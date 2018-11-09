#include <AMReX_ParmParse.H>
#include <AMReX_BC_TYPES.H>
#include <AMReX_Box.H>

#include <incflo.H>
#include <boundary_conditions_F.H>
#include <embedded_boundaries_F.H>
#include <setup_F.H>

void incflo::ReadParameters()
{
    {
        // Variables without prefix in inputs file
		ParmParse pp;

		pp.query("stop_time", stop_time);
		pp.query("max_step", max_step);
		pp.query("steady_state", steady_state);
    }
	{
        // Prefix amr
		ParmParse pp("amr");

		pp.query("regrid_int", regrid_int);
        pp.query("refine_cutcells", refine_cutcells);

		pp.query("check_file", check_file);
		pp.query("check_int", check_int);
		pp.query("restart", restart_file);

		pp.query("plot_file", plot_file);
		pp.query("plot_int", plot_int);

        // TODO: maybe move this?
		pp.query("write_eb_surface", write_eb_surface);
	}
	{
        // Prefix incflo
		ParmParse pp("incflo");

        pp.query("verbose", verbose);
		pp.query("cfl", cfl);
		pp.query("fixed_dt", fixed_dt);
		pp.query("steady_state_tol", steady_state_tol);
        pp.query("nodal_pressure", nodal_pressure);
		pp.query("explicit_diffusion", explicit_diffusion);

        // Physics
		pp.queryarr("gravity", gravity, 0, 3);
        pp.query("ro_0", ro_0);
        AMREX_ALWAYS_ASSERT(ro_0 >= 0.0);

        // Fluid properties
        pp.query("mu", mu);
        AMREX_ALWAYS_ASSERT(mu > 0.0);

        fluid_model = "newtonian";
        pp.query("fluid_model", fluid_model);
        if(fluid_model == "newtonian")
        {
            amrex::Print() << "Newtonian fluid with"
                           << " mu = " << mu << std::endl;
        }
        else if(fluid_model == "powerlaw")
        {
            pp.query("n", n);
            AMREX_ALWAYS_ASSERT(n > 0.0);
            AMREX_ALWAYS_ASSERT_WITH_MESSAGE(n != 1.0,
                    "No point in using power-law rheology with n = 1");

            amrex::Print() << "Power-law fluid with"
                           << " mu = " << mu
                           << ", n = " << n <<  std::endl;
        }
        else if(fluid_model == "bingham")
        {
            pp.query("tau_0", tau_0);
            AMREX_ALWAYS_ASSERT_WITH_MESSAGE(tau_0 > 0.0,
                    "No point in using Bingham rheology with tau_0 = 0");

            pp.query("papa_reg", papa_reg);
            AMREX_ALWAYS_ASSERT_WITH_MESSAGE(papa_reg > 0.0,
                    "Papanastasiou regularisation parameter must be positive");

            amrex::Print() << "Bingham fluid with"
                           << " mu = " << mu
                           << ", tau_0 = " << tau_0
                           << ", papa_reg = " << papa_reg << std::endl;
        }
        else if(fluid_model == "hb")
        {
            pp.query("n", n);
            AMREX_ALWAYS_ASSERT(n > 0.0);
            AMREX_ALWAYS_ASSERT_WITH_MESSAGE(n != 1.0,
                    "No point in using Herschel-Bulkley rheology with n = 1");

            pp.query("tau_0", tau_0);
            AMREX_ALWAYS_ASSERT_WITH_MESSAGE(tau_0 > 0.0,
                    "No point in using Herschel-Bulkley rheology with tau_0 = 0");

            pp.query("papa_reg", papa_reg);
            AMREX_ALWAYS_ASSERT_WITH_MESSAGE(papa_reg > 0.0,
                    "Papanastasiou regularisation parameter must be positive");

            amrex::Print() << "Herschel-Bulkley fluid with"
                           << " mu = " << mu
                           << ", n = " << n
                           << ", tau_0 = " << tau_0
                           << ", papa_reg = " << papa_reg << std::endl;
        }
        else if(fluid_model == "smd")
        {
            pp.query("n", n);
            AMREX_ALWAYS_ASSERT(n > 0.0);

            pp.query("tau_0", tau_0);
            AMREX_ALWAYS_ASSERT_WITH_MESSAGE(tau_0 > 0.0,
                    "No point in using de Souza Mendes-Dutra rheology with tau_0 = 0");

            pp.query("eta_0", eta_0);
            AMREX_ALWAYS_ASSERT(eta_0 > 0.0);

            amrex::Print() << "de Souza Mendes-Dutra fluid with"
                           << " mu = " << mu
                           << ", n = " << n
                           << ", tau_0 = " << tau_0
                           << ", eta_0 = " << eta_0 << std::endl;
        }
        else
        {
            amrex::Abort("Unknown fluid_model! Choose either newtonian, powerlaw, bingham, hb, smd");
        }

		// Option to control MLMG behavior
		pp.query("mg_verbose", mg_verbose);
		pp.query("mg_cg_verbose", mg_cg_verbose);
		pp.query("mg_max_iter", mg_max_iter);
		pp.query("mg_cg_maxiter", mg_cg_maxiter);
		pp.query("mg_max_fmg_iter", mg_max_fmg_iter);
		pp.query("mg_rtol", mg_rtol);
		pp.query("mg_atol", mg_atol);

        // Default bottom solver is bicgstab, but alternatives are "smoother" or "hypre"
        bottom_solver_type = "bicgstab";
        pp.query( "bottom_solver_type",  bottom_solver_type );

        // Loads constants given at runtime `inputs` file into the Fortran module "constant"
        incflo_get_data(gravity.dataPtr(), &ro_0, &mu, &n, &tau_0, &papa_reg, &eta_0);
	}
}

void incflo::Init()
{
	InitIOData();

    // Define coarse level BoxArray and DistributionMap
	const BoxArray& ba = MakeBaseGrids();
	DistributionMapping dm(ba, ParallelDescriptor::NProcs());

    // The time t is a member variable initalized to zero, and has not been changed yet. 
	MakeNewLevelFromScratch(0, t, ba, dm);

    //TODO : get rid of this stuff and add call to AmrCore::InitFromScratch(t). 
    finest_level = max_level;
    for(int lev = 1; lev <= max_level; lev++)
    {
        // This refines the central half of the domain
        int ilo = ba[0].size()[0] / 2;
        int ihi = 3 * ba[0].size()[0] / 2 - 1;
        int jlo = ba[0].size()[1] / 2;
        int jhi = 3 * ba[0].size()[1] / 2 - 1;
        int klo = ba[0].size()[2] / 2;
        int khi = 3 * ba[0].size()[2] / 2 - 1;
        IntVect lo(ilo, jlo, klo);
        IntVect hi(ihi, jhi, khi);
        Box bx(lo, hi);
        BoxArray ba_ref(bx);

        amrex::Print() << "Setting refined region to " << ba_ref << std::endl;

        DistributionMapping dm_ref(ba_ref, ParallelDescriptor::NProcs());
        MakeNewLevelFromScratch(lev, t, ba_ref, dm_ref);
    }

    // We only do these at level 0
	int cyc_x = 0, cyc_y = 0, cyc_z = 0;
    if(geom[0].isPeriodic(0)) cyc_x = 1;
    if(geom[0].isPeriodic(1)) cyc_y = 1;
    if(geom[0].isPeriodic(2)) cyc_z = 1;
	incflo_set_cyclic(&cyc_x, &cyc_y, &cyc_z);

    for(int lev = 0; lev <= max_level; lev++)
    {
        incflo_set_bc_type(lev);
    }

	// Create MAC projection object
	mac_projection.reset(new MacProjection(this, nghost, &ebfactory));
	mac_projection->set_bcs(bc_ilo, bc_ihi, bc_jlo, bc_jhi, bc_klo, bc_khi);
}

// This function initializes the attributes vecVarsName,
//                                          pltscalarVars, pltscaVarsName,
//                                          chkscalarVars, chkscaVarsName.
// If new variables need to be added to the output/checkpoint, simply add them
// here and the IO routines will automatically take care of them.
void incflo::InitIOData()
{
	// Define the list of vector variables on faces that need to be written
	// to plotfile/checkfile.
	vecVarsName = {"velx", "vely", "velz", "gpx", "gpy", "gpz"};

	// Define the list of scalar variables at cell centers that need to be
	// written to plotfile/checkfile. "volfrac" MUST always be last without any
	// mf associated to it!!!
	pltscaVarsName = {"p", "ro", "eta", "strainrate", "stress", "vort", "divu", "volfrac"};
	pltscalarVars = {&p, &ro, &eta, &strainrate, &strainrate, &vort, &divu};

	chkscaVarsName = {"p", "ro", "eta"};
	chkscalarVars = {&p, &ro, &eta};
}

BoxArray incflo::MakeBaseGrids() const
{
	BoxArray ba(geom[0].Domain());

	ba.maxSize(max_grid_size[0]);

	// We only call ChopGrids if dividing up the grid using max_grid_size didn't
	//    create enough grids to have at least one grid per processor.
	// This option is controlled by "refine_grid_layout" which defaults to true.

    if(refine_grid_layout && ba.size() < ParallelDescriptor::NProcs())
		ChopGrids(geom[0].Domain(), ba, ParallelDescriptor::NProcs());

	if(ba == grids[0])
		ba = grids[0]; // to avoid dupliates

	amrex::Print() << "In MakeBaseGrids: BA HAS " << ba.size() << " GRIDS " << std::endl;
	return ba;
}

void incflo::ChopGrids(const Box& domain, BoxArray& ba, int target_size) const
{
	if(ParallelDescriptor::IOProcessor())
		amrex::Warning(
			"Using max_grid_size only did not make enough grids for the number of processors");

	// Here we hard-wire the maximum number of times we divide the boxes.
	int max_div = 10;

	// Here we hard-wire the minimum size in any one direction the boxes can be
	int min_grid_size = 4;

	IntVect chunk(domain.length(0), domain.length(1), domain.length(2));

	int j;
	for(int cnt = 1; cnt <= max_div; ++cnt)
	{
		if(chunk[0] >= chunk[1] && chunk[0] >= chunk[2])
		{
			j = 0;
		}
		else if(chunk[1] >= chunk[0] && chunk[1] >= chunk[2])
		{
			j = 1;
		}
		else if(chunk[2] >= chunk[0] && chunk[2] >= chunk[1])
		{
			j = 2;
		}
		chunk[j] /= 2;

		if(chunk[j] >= min_grid_size)
		{
			ba.maxSize(chunk);
		}
		else
		{
			// chunk[j] was the biggest chunk -- if this is too small then we're done
			if(ParallelDescriptor::IOProcessor())
				amrex::Warning(
					"ChopGrids was unable to make enough grids for the number of processors");
			return;
		}

		// Test if we now have enough grids
		if(ba.size() >= target_size)
			return;
	}
}

// Only used when restarting from checkpoint file
// Might be deprecated after we implement RemakeLevel
void incflo::ReMakeNewLevelFromScratch(int lev,
                                       const BoxArray& new_grids,
                                       const DistributionMapping& new_dmap)
{
	SetBoxArray(lev, new_grids);
	SetDistributionMap(lev, new_dmap);

    if(lev == 0) MakeBCArrays();

	// We need to re-fill these arrays for the larger domain (after replication).
	incflo_set_bc_type(lev);
}

void incflo::InitLevelData()
{
	// This needs is needed before initializing level MultiFabs: ebfactories should
	// not change after the eb-dependent MultiFabs are allocated.
	make_eb_geometry();

	// Allocate the fluid data, NOTE: this depends on the ebfactories.
    for(int lev = 0; lev <= max_level; lev++)
        AllocateArrays(lev);
}

void incflo::PostInit(int restart_flag)
{
    // Initial fluid arrays: pressure, velocity, density, viscosity
    incflo_init_fluid(restart_flag);
}

void incflo::MakeBCArrays()
{
	bc_ilo.resize(max_level + 1);
	bc_ihi.resize(max_level + 1);
	bc_jlo.resize(max_level + 1);
	bc_jhi.resize(max_level + 1);
	bc_klo.resize(max_level + 1);
	bc_khi.resize(max_level + 1);

    for(int lev = 0; lev <= max_level; lev++)
    {
        // Define and allocate the integer MultiFab that is the outside adjacent cells of the
        // problem domain.
        Box domainx(geom[lev].Domain());
        domainx.grow(1, nghost);
        domainx.grow(2, nghost);
        Box box_ilo = amrex::adjCellLo(domainx, 0, 1);
        Box box_ihi = amrex::adjCellHi(domainx, 0, 1);

        Box domainy(geom[lev].Domain());
        domainy.grow(0, nghost);
        domainy.grow(2, nghost);
        Box box_jlo = amrex::adjCellLo(domainy, 1, 1);
        Box box_jhi = amrex::adjCellHi(domainy, 1, 1);

        Box domainz(geom[lev].Domain());
        domainz.grow(0, nghost);
        domainz.grow(1, nghost);
        Box box_klo = amrex::adjCellLo(domainz, 2, 1);
        Box box_khi = amrex::adjCellHi(domainz, 2, 1);

        // Note that each of these is a single IArrayBox so every process has a copy of them
        bc_ilo[lev].reset(new IArrayBox(box_ilo, 2));
        bc_ihi[lev].reset(new IArrayBox(box_ihi, 2));
        bc_jlo[lev].reset(new IArrayBox(box_jlo, 2));
        bc_jhi[lev].reset(new IArrayBox(box_jhi, 2));
        bc_klo[lev].reset(new IArrayBox(box_klo, 2));
        bc_khi[lev].reset(new IArrayBox(box_khi, 2));
    }
}

void incflo::incflo_init_fluid(int is_restarting)
{
	Real xlen = geom[0].ProbHi(0) - geom[0].ProbLo(0);
	Real ylen = geom[0].ProbHi(1) - geom[0].ProbLo(1);
	Real zlen = geom[0].ProbHi(2) - geom[0].ProbLo(2);

    // Here we set bc values for p and u,v,w before the IC's are set
    incflo_set_bc0();

    for(int lev = 0; lev <= max_level; lev++)
    {
        Box domain(geom[lev].Domain());

        Real dx = geom[lev].CellSize(0);
        Real dy = geom[lev].CellSize(1);
        Real dz = geom[lev].CellSize(2);

        // We deliberately don't tile this loop since we will be looping
        //    over bc's on faces and it makes more sense to do this one grid at a time
        for(MFIter mfi(*ro[lev], false); mfi.isValid(); ++mfi)
        {
            const Box& bx = mfi.validbox();
            const Box& sbx = (*ro[lev])[mfi].box();

            if(is_restarting)
            {
                init_fluid_restart(sbx.loVect(), sbx.hiVect(),
                                   bx.loVect(), bx.hiVect(),
                                   (*eta[lev])[mfi].dataPtr());
            }
            else
            {
                init_fluid(sbx.loVect(), sbx.hiVect(),
                           bx.loVect(), bx.hiVect(),
                           domain.loVect(), domain.hiVect(),
                           (*ro[lev])[mfi].dataPtr(),
                           (*p[lev])[mfi].dataPtr(),
                           (*vel[lev])[mfi].dataPtr(),
                           (*eta[lev])[mfi].dataPtr(),
                           &dx, &dy, &dz,
                           &xlen, &ylen, &zlen);
            }
        }
    }

    incflo_set_p0();

    // Here we re-set the bc values for p and u,v,w just in case init_fluid
    //      over-wrote some of the bc values with ic values
    incflo_set_bc0();

    for(int lev = 0; lev <= max_level; lev++)
    {
        Box domain(geom[lev].Domain());

        if(!nodal_pressure)
            incflo_extrap_pressure(lev, p0[lev]);

        fill_mf_bc(lev, *ro[lev]);
        vel[lev]->FillBoundary(geom[lev].periodicity());
        fill_mf_bc(lev, *eta[lev]);

        if(is_restarting)
        {
            if(!nodal_pressure)
                incflo_extrap_pressure(lev, p[lev]);
        }
    }

    if(!is_restarting)
    {
        // Here initialize dt to -1 so that we don't check new dt against a previous value
        dt = -1.;
        incflo_set_scalar_bcs();

        // Project the initial velocity field
        incflo_initial_projection();

        // Iterate to compute the initial pressure
        incflo_initial_iterations();
    }
}

void incflo::incflo_set_bc_type(int lev)
{
	Real dx = geom[lev].CellSize(0);
	Real dy = geom[lev].CellSize(1);
	Real dz = geom[lev].CellSize(2);
	Real xlen = geom[lev].ProbHi(0) - geom[lev].ProbLo(0);
	Real ylen = geom[lev].ProbHi(1) - geom[lev].ProbLo(1);
	Real zlen = geom[lev].ProbHi(2) - geom[lev].ProbLo(2);
	Box domain(geom[lev].Domain());

	set_bc_type(bc_ilo[lev]->dataPtr(),
				bc_ihi[lev]->dataPtr(),
				bc_jlo[lev]->dataPtr(),
				bc_jhi[lev]->dataPtr(),
				bc_klo[lev]->dataPtr(),
				bc_khi[lev]->dataPtr(),
				domain.loVect(),
				domain.hiVect(),
				&dx,
				&dy,
				&dz,
				&xlen,
				&ylen,
				&zlen,
				&nghost);
}

void incflo::incflo_set_bc0()
{
    for(int lev = 0; lev <= max_level; lev++)
    {
        Box domain(geom[lev].Domain());

        // Don't tile this -- at least for now
        for(MFIter mfi(*ro[lev]); mfi.isValid(); ++mfi)
        {
            const Box& sbx = (*ro[lev])[mfi].box();

            set_bc0(sbx.loVect(),
                    sbx.hiVect(),
                    (*ro[lev])[mfi].dataPtr(),
                    (*eta[lev])[mfi].dataPtr(),
                    bc_ilo[lev]->dataPtr(),
                    bc_ihi[lev]->dataPtr(),
                    bc_jlo[lev]->dataPtr(),
                    bc_jhi[lev]->dataPtr(),
                    bc_klo[lev]->dataPtr(),
                    bc_khi[lev]->dataPtr(),
                    domain.loVect(),
                    domain.hiVect(),
                    &nghost, &nodal_pressure);
        }

        if(!nodal_pressure)
            fill_mf_bc(lev, *p[lev]);
        fill_mf_bc(lev, *ro[lev]);
    }

    // Put velocity Dirichlet bc's on faces
    int extrap_dir_bcs = 0;
    incflo_set_velocity_bcs(t, extrap_dir_bcs);

    for(int lev = 0; lev <= max_level; lev++)
    {
        vel[lev]->FillBoundary(geom[lev].periodicity());
    }
}

void incflo::incflo_set_p0()
{
	Real xlen = geom[0].ProbHi(0) - geom[0].ProbLo(0);
	Real ylen = geom[0].ProbHi(1) - geom[0].ProbLo(1);
	Real zlen = geom[0].ProbHi(2) - geom[0].ProbLo(2);

	int delp_dir;
	set_delp_dir(&delp_dir);

    IntVect press_per = IntVect(geom[0].isPeriodic(0),
                                geom[0].isPeriodic(1),
                                geom[0].isPeriodic(2));

	// Here we set a separate periodicity flag for p0 because when we use
	// pressure drop (delp) boundary conditions we fill all variables *except* p0
	// periodically
	if(delp_dir > -1)
		press_per[delp_dir] = 0;
	p0_periodicity = Periodicity(press_per);

    for(int lev = 0; lev <= max_level; lev++)
    {
        Real dx = geom[lev].CellSize(0);
        Real dy = geom[lev].CellSize(1);
        Real dz = geom[lev].CellSize(2);

        Box domain(geom[lev].Domain());

        // We deliberately don't tile this loop since we will be looping
        //    over bc's on faces and it makes more sense to do this one grid at a time
        for(MFIter mfi(*ro[lev], false); mfi.isValid(); ++mfi)
        {

            const Box& bx = mfi.validbox();

            set_p0(bx.loVect(), bx.hiVect(),
                   domain.loVect(), domain.hiVect(),
                   BL_TO_FORTRAN_ANYD((*p0[lev])[mfi]),
                   BL_TO_FORTRAN_ANYD((*gp0[lev])[mfi]),
                   &dx, &dy, &dz,
                   &xlen, &ylen, &zlen,
                   &delp_dir,
                   bc_ilo[lev]->dataPtr(),
                   bc_ihi[lev]->dataPtr(),
                   bc_jlo[lev]->dataPtr(),
                   bc_jhi[lev]->dataPtr(),
                   bc_klo[lev]->dataPtr(),
                   bc_khi[lev]->dataPtr(),
                   &nghost, &nodal_pressure);
        }

        p0[lev]->FillBoundary(p0_periodicity);
        gp0[lev]->FillBoundary(p0_periodicity);
    }
}

//
// Perform initial pressure iterations
//
void incflo::incflo_initial_iterations()
{
    int initialisation = 1;
	incflo_compute_dt(initialisation);

	amrex::Print() << "Doing initial pressure iterations with dt = " << dt << std::endl;

    // Fill ghost cells
    incflo_set_scalar_bcs();
    incflo_set_velocity_bcs(t, 0);

    // Copy vel into vel_o
    for(int lev = 0; lev <= finest_level; lev++)
    {
        MultiFab::Copy(*vel_o[lev], *vel[lev], 0, 0, vel[lev]->nComp(), vel_o[lev]->nGrow());
    }

	//  Create temporary multifabs to hold conv and divtau
	Vector<std::unique_ptr<MultiFab>> conv;
	Vector<std::unique_ptr<MultiFab>> divtau;
    conv.resize(finest_level + 1);
    divtau.resize(finest_level + 1);
    for(int lev = 0; lev <= finest_level; lev++)
    {
        conv[lev].reset(new MultiFab(grids[lev], dmap[lev], 3, 0, MFInfo(), *ebfactory[lev]));
        divtau[lev].reset(new MultiFab(grids[lev], dmap[lev], 3, 0, MFInfo(), *ebfactory[lev]));
    }

    bool proj_2 = false;
	for(int iter = 0; iter < 3; ++iter)
	{
		amrex::Print() << "\n In initial_iterations: iter = " << iter << "\n";

		incflo_apply_predictor(conv, divtau, proj_2);

        for(int lev = 0; lev <= finest_level; lev++)
        {
            // Replace vel by the original values
            MultiFab::Copy(*vel[lev], *vel_o[lev], 0, 0, vel[lev]->nComp(), vel[lev]->nGrow());
        }
        // Reset the boundary values (necessary if they are time-dependent)
        incflo_set_velocity_bcs(t, 0);
	}
}

void incflo::incflo_initial_projection()
{
    // Project velocity field to make sure initial velocity is divergence-free
	amrex::Print() << "Initial projection:" << std::endl;

	// Need to add this call here so that the MACProjection internal arrays
	//  are allocated so that the cell-centered projection can use the MAC
	//  data structures and set_velocity_bcs routine
	mac_projection->update_internals();

	bool proj_2 = true;
	Real dummy_dt = 1.0;
	incflo_apply_projection(t, dummy_dt, proj_2);

	// We initialize p and gp back to zero (p0 may still be still non-zero)
    for(int lev = 0; lev <= finest_level; lev++)
    {
        p[lev]->setVal(0.0);
        gp[lev]->setVal(0.0);
    }
}


