#include "amr-wind/utilities/sampling/FreeSurface.H"
#include "amr-wind/utilities/io_utils.H"
#include <AMReX_MultiFabUtil.H>
#include <utility>
#include "amr-wind/utilities/ncutils/nc_interface.H"

#include "AMReX_ParmParse.H"

namespace amr_wind {
namespace free_surface {

FreeSurface::FreeSurface(CFDSim& sim, std::string label)
    : m_sim(sim), m_label(std::move(label)), m_vof(sim.repo().get_field("vof"))
{
#ifdef AMREX_USE_GPU
    amrex::Print() << "WARNING: FreeSurface: Running on GPUs..." << std::endl;
#endif
}

FreeSurface::~FreeSurface() = default;

void FreeSurface::initialize()
{
    BL_PROFILE("amr-wind::FreeSurface::initialize");

    {
        amrex::ParmParse pp(m_label);
        pp.query("output_frequency", m_out_freq);
        pp.query("output_format", m_out_fmt);
        // Load parameters of freesurface sampling
        pp.query("num_instances", m_ninst);
        pp.getarr("num_points", m_npts_dir);
        pp.getarr("start", m_start);
        pp.getarr("end", m_end);
        AMREX_ALWAYS_ASSERT(static_cast<int>(m_start.size()) == AMREX_SPACEDIM);
        AMREX_ALWAYS_ASSERT(static_cast<int>(m_end.size()) == AMREX_SPACEDIM);
        AMREX_ALWAYS_ASSERT(static_cast<int>(m_npts_dir.size()) == 2);
    }

    // Calculate total number of points
    m_npts = m_npts_dir[0] * m_npts_dir[1];

    // Turn parameters into 2D grid
    m_locs.resize(m_npts);
    m_out.resize(m_npts * m_ninst);

    // Get size of spacing
    amrex::Vector<amrex::Real> dx = {0.0, 0.0};
    for (int nd = 0; nd < 2; ++nd) {
        int d = m_griddim[nd];
        dx[nd] = (m_end[d] - m_start[d]) / amrex::max(m_npts_dir[nd] - 1, 1);
    }

    // Store locations
    int idx = 0;
    for (int j = 0; j < m_npts_dir[1]; ++j) {
        for (int i = 0; i < m_npts_dir[0]; ++i) {
            // Initialize output values to 0.0
            for (int ni = 0; ni < m_ninst; ++ni) {
                m_out[idx * m_ninst + ni] = m_start[m_orient];
            }
            for (int nd = 0; nd < 2; ++nd) {
                int d = m_griddim[nd];
                m_locs[idx][nd] =
                    m_start[d] + dx[nd] * (i * (1 - nd) + j * (nd));
            }
            ++idx;
        }
    }

    if (m_out_fmt == "netcdf") {
        prepare_netcdf_file();
    }
}

void FreeSurface::post_advance_work()
{

    BL_PROFILE("amr-wind::FreeSurface::post_advance_work");
    const auto& time = m_sim.time();
    const int tidx = time.time_index();
    // Skip processing if it is not an output timestep
    if (!(tidx % m_out_freq == 0)) {
        return;
    }

    // Zero data in output array
    for (int n = 0; n < m_npts * m_ninst; n++) {
        m_out[n] = 0.0;
    }
    // Set up device vector of outputs, initialize to above phi0
    const auto& plo0 = m_sim.mesh().Geom(0).ProbLoArray();
    const auto& phi0 = m_sim.mesh().Geom(0).ProbHiArray();
    amrex::Gpu::DeviceVector<double> dout(m_npts, phi0[2] + 1.0);
    auto* dout_ptr = dout.data();

    // Sum of interface locations at each point (assumes one interface only)
    const int finest_level = m_vof.repo().num_active_levels() - 1;

    // Loop instances
    for (int ni = 0; ni < m_ninst; ++ni) {
        for (int lev = 0; lev <= finest_level; lev++) {

            // Use level_mask to identify smallest volume
            amrex::iMultiFab level_mask;
            if (lev < finest_level) {
                level_mask = makeFineMask(
                    m_sim.mesh().boxArray(lev),
                    m_sim.mesh().DistributionMap(lev),
                    m_sim.mesh().boxArray(lev + 1), amrex::IntVect(2), 1, 0);
            } else {
                level_mask.define(
                    m_sim.mesh().boxArray(lev),
                    m_sim.mesh().DistributionMap(lev), 1, 0, amrex::MFInfo());
                level_mask.setVal(1);
            }

            const auto& vof = m_vof(lev);
            const auto& geom = m_sim.mesh().Geom(lev);
            const amrex::GpuArray<amrex::Real, AMREX_SPACEDIM> dx =
                geom.CellSizeArray();
            const amrex::GpuArray<amrex::Real, AMREX_SPACEDIM> dxi =
                geom.InvCellSizeArray();
            const amrex::GpuArray<amrex::Real, AMREX_SPACEDIM> plo =
                geom.ProbLoArray();

            // Loop points in 2D grid
            for (int n = 0; n < m_npts; ++n) {
                amrex::GpuArray<amrex::Real, 2> loc;
                for (int d = 0; d < 2; ++d) {
                    loc[d] = m_locs[n][d];
                }

                m_out[ni * m_npts + n] = amrex::max(
                    m_out[ni * m_npts + n],
                    amrex::ReduceMax(
                        vof, level_mask, 0,
                        [=] AMREX_GPU_HOST_DEVICE(
                            amrex::Box const& bx,
                            amrex::Array4<amrex::Real const> const& vof_arr,
                            amrex::Array4<int const> const& mask_arr)
                            -> amrex::Real {
                            amrex::Real height_fab = 0.0;

                            amrex::Loop(
                                bx,
                                [=, &height_fab](int i, int j, int k) noexcept {
                                    // Initialize height measurement
                                    amrex::Real ht = plo[2];
                                    // Cell location
                                    amrex::GpuArray<amrex::Real, AMREX_SPACEDIM>
                                        xm;
                                    xm[0] = plo[0] + (i + 0.5) * dx[0];
                                    xm[1] = plo[1] + (j + 0.5) * dx[1];
                                    xm[2] = plo[2] + (k + 0.5) * dx[2];
                                    // (1) Check that cell height is below
                                    // previous instance. (2) Check if cell
                                    // contains 2D grid point: complicated
                                    // conditional is to avoid double-counting
                                    // and includes exception for lo boundary.
                                    // (3) Check if cell is obviously
                                    // multiphase, then check if cell might have
                                    // interface at top or bottom
                                    if ((dout_ptr[n] > xm[2] + 0.5 * dx[2]) &&
                                        (((plo[0] == loc[0] &&
                                           xm[0] - loc[0] == 0.5 * dx[0]) ||
                                          (xm[0] - loc[0] < 0.5 * dx[0] &&
                                           loc[0] - xm[0] <= 0.5 * dx[0])) &&
                                         ((plo[1] == loc[1] &&
                                           xm[1] - loc[1] == 0.5 * dx[1]) ||
                                          (xm[1] - loc[1] < 0.5 * dx[1] &&
                                           loc[1] - xm[1] <= 0.5 * dx[1]))) &&
                                        ((vof_arr(i, j, k) < (1.0 - 1e-12) &&
                                          vof_arr(i, j, k) > 1e-12) ||
                                         (vof_arr(i, j, k) < 1e-12 &&
                                          (vof_arr(i, j, k + 1) >
                                               (1.0 - 1e-12) ||
                                           vof_arr(i, j, k - 1) >
                                               (1.0 - 1e-12))))) {
                                        // Interpolate in x and y for the
                                        // current cell and the ones above and
                                        // below
                                        amrex::Real wx_hi;
                                        amrex::Real wy_hi;
                                        int iup, idn, jup, jdn;

                                        // Determine which cells to use for x, y
                                        if (loc[0] < xm[0]) {
                                            iup = i;
                                            idn = i - 1;
                                            wx_hi = (loc[0] - (xm[0] - dx[0])) *
                                                    dxi[0];
                                        } else {
                                            iup = i + 1;
                                            idn = i;
                                            wx_hi = (loc[0] - xm[0]) * dxi[0];
                                        }
                                        if (loc[1] < xm[1]) {
                                            jup = j;
                                            jdn = j - 1;
                                            wy_hi = (loc[1] - (xm[1] - dx[1])) *
                                                    dxi[1];
                                        } else {
                                            jup = j + 1;
                                            jdn = j;
                                            wy_hi = (loc[1] - xm[1]) * dxi[1];
                                        }
                                        amrex::Real wx_lo = 1.0 - wx_hi;
                                        amrex::Real wy_lo = 1.0 - wy_hi;

                                        amrex::Real vof_above =
                                            wx_lo * wy_lo *
                                                vof_arr(idn, jdn, k + 1) +
                                            wx_lo * wy_hi *
                                                vof_arr(idn, jup, k + 1) +
                                            wx_hi * wy_lo *
                                                vof_arr(iup, jdn, k + 1) +
                                            wx_hi * wy_hi *
                                                vof_arr(iup, jup, k + 1);
                                        amrex::Real vof_here =
                                            wx_lo * wy_lo *
                                                vof_arr(idn, jdn, k) +
                                            wx_lo * wy_hi *
                                                vof_arr(idn, jup, k) +
                                            wx_hi * wy_lo *
                                                vof_arr(iup, jdn, k) +
                                            wx_hi * wy_hi *
                                                vof_arr(iup, jup, k);
                                        amrex::Real vof_below =
                                            wx_lo * wy_lo *
                                                vof_arr(idn, jdn, k - 1) +
                                            wx_lo * wy_hi *
                                                vof_arr(idn, jup, k - 1) +
                                            wx_hi * wy_lo *
                                                vof_arr(iup, jdn, k - 1) +
                                            wx_hi * wy_hi *
                                                vof_arr(iup, jup, k - 1);

                                        // Determine which cell to
                                        // interpolate with
                                        bool above = (vof_above - 0.5) *
                                                         (vof_here - 0.5) <=
                                                     0.0;
                                        bool below = (vof_below - 0.5) *
                                                         (vof_here - 0.5) <=
                                                     0.0;
                                        if (above) {
                                            // Interpolate positive
                                            // direction
                                            ht = xm[2] +
                                                 (dx[2]) /
                                                     (vof_above - vof_here) *
                                                     (0.5 - vof_here);
                                        } else {
                                            if (below) {
                                                // Interpolate negative
                                                // direction
                                                ht =
                                                    xm[2] -
                                                    (dx[2]) /
                                                        (vof_below - vof_here) *
                                                        (0.5 - vof_here);
                                            }
                                            // If none satisfy requirement, then
                                            // the isosurface vof = 0.5 cannot
                                            // be detected in the z-direction
                                        }
                                    }
                                    // Offset by removing lo and contribute to
                                    // whole
                                    height_fab = amrex::max(
                                        height_fab,
                                        mask_arr(i, j, k) * (ht - plo[2]));
                                });
                            return height_fab;
                        }));
            }
        }

        // Loop points in 2D grid
        for (int n = 0; n < m_npts; n++) {
            amrex::ParallelDescriptor::ReduceRealMax(m_out[ni * m_npts + n]);
        }
        // Add problo back to heights, making them absolute, not relative
        for (int n = 0; n < m_npts; n++) {
            m_out[ni * m_npts + n] += plo0[2];
        }
        // Copy last m_out to device vector
        amrex::Gpu::copy(
            amrex::Gpu::hostToDevice, &m_out[ni * m_npts],
            &m_out[(ni + 1) * m_npts - 1] + 1, dout.begin());
    }

    process_output();
}

void FreeSurface::process_output()
{
    if (m_out_fmt == "ascii") {
        write_ascii();
    } else if (m_out_fmt == "netcdf") {
        write_netcdf();
    } else {
        amrex::Abort("FreeSurface: Invalid output format encountered");
    }
}

void FreeSurface::write_ascii()
{
    BL_PROFILE("amr-wind::FreeSurface::write_ascii");
    amrex::Print()
        << "WARNING: FreeSurface: ASCII output will impact performance"
        << std::endl;

    const std::string post_dir = "post_processing";
    const std::string sname =
        amrex::Concatenate(m_label, m_sim.time().time_index());

    if (!amrex::UtilCreateDirectory(post_dir, 0755)) {
        amrex::CreateDirectoryFailed(post_dir);
    }
    const std::string fname = post_dir + "/" + sname + ".txt";

    if (amrex::ParallelDescriptor::IOProcessor()) {
        //
        // Have I/O processor open file and write everything.
        //
        std::ofstream File;

        File.open(fname.c_str(), std::ios::out | std::ios::trunc);

        if (!File.good()) {
            amrex::FileOpenFailed(fname);
        }

        // Metadata
        File << m_npts << '\n';
        File << m_npts_dir[0] << ' ' << m_npts_dir[1] << '\n';

        // Points in grid (x, y, z0, z1, ...)
        for (int n = 0; n < m_npts; ++n) {
            File << m_locs[n][0] << ' ' << m_locs[n][1];
            for (int ni = 0; ni < m_ninst; ++ni) {
                File << ' ' << m_out[ni * m_npts + n];
            }
            File << '\n';
        }

        File.flush();

        File.close();

        if (!File.good()) {
            amrex::Abort("FreeSurface::write_ascii(): problem writing file");
        }
    }
}

void FreeSurface::prepare_netcdf_file()
{
#ifdef AMR_WIND_USE_NETCDF

    const std::string post_dir = "post_processing";
    const std::string sname =
        amrex::Concatenate(m_label, m_sim.time().time_index());
    if (!amrex::UtilCreateDirectory(post_dir, 0755)) {
        amrex::CreateDirectoryFailed(post_dir);
    }
    m_ncfile_name = post_dir + "/" + sname + ".nc";

    // Only I/O processor handles NetCDF generation
    if (!amrex::ParallelDescriptor::IOProcessor()) return;

    auto ncf = ncutils::NCFile::create(m_ncfile_name, NC_CLOBBER | NC_NETCDF4);
    const std::string nt_name = "num_time_steps";
    const std::string ngp_name = "num_grid_points";
    const std::string ninst_name = "num_instances";
    ncf.enter_def_mode();
    ncf.put_attr("title", "AMR-Wind data sampling output");
    ncf.put_attr("version", ioutils::amr_wind_version());
    ncf.put_attr("created_on", ioutils::timestamp());
    ncf.def_dim(nt_name, NC_UNLIMITED);
    ncf.def_dim(ngp_name, m_npts);
    ncf.def_dim(ninst_name, m_ninst);
    ncf.def_dim("ndim2", 2);
    ncf.def_var("time", NC_DOUBLE, {nt_name});

    // Metadata related to the 2D grid used to sample
    const std::vector<int> ijk{m_npts_dir[0], m_npts_dir[1]};
    ncf.put_attr("ijk_dims", ijk);
    ncf.put_attr("start", m_start);
    ncf.put_attr("end", m_end);

    // Set up array of data for locations in 2D grid
    ncf.def_var("coordinates2D", NC_DOUBLE, {ngp_name, "ndim2"});

    // Set up array for height outputs
    ncf.def_var("heights", NC_DOUBLE, {nt_name, ninst_name, ngp_name});

    ncf.exit_def_mode();

    // Populate data that doesn't change
    const std::vector<size_t> start{0, 0};
    std::vector<size_t> count{0, 2};
    count[0] = m_npts;
    auto xy = ncf.var("coordinates2D");
    xy.put(&m_locs[0][0], start, count);

#else
    amrex::Abort(
        "NetCDF support was not enabled during build time. Please "
        "recompile or "
        "use native format");
#endif
}

void FreeSurface::write_netcdf()
{
#ifdef AMR_WIND_USE_NETCDF

    if (!amrex::ParallelDescriptor::IOProcessor()) return;
    auto ncf = ncutils::NCFile::open(m_ncfile_name, NC_WRITE);
    const std::string nt_name = "num_time_steps";
    // Index of the next timestep
    const size_t nt = ncf.dim(nt_name).len();
    {
        auto time = m_sim.time().new_time();
        ncf.var("time").put(&time, {nt}, {1});
    }

    std::vector<size_t> start{nt, 0, 0};
    std::vector<size_t> count{1, 0, 0};

    count[1] = 1;
    count[2] = m_npts;
    auto var = ncf.var("heights");
    for (int ni = 0; ni < m_ninst; ++ni) {
        var.put(&m_out[ni * m_npts], start, count);
        ++start[1];
    }

    ncf.close();
#endif
}

} // namespace free_surface
} // namespace amr_wind
