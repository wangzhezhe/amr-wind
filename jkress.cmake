cmake -C ../../ascent/jkress-enterprise.cmake  ../ -DCMAKE_INSTALL_PREFIX=../install -DCMAKE_BUILD_TYPE=Release -DAMR_WIND_ENABLE_ASCENT=ON -DAMR_WIND_ENABLE_MPI=ON  -DAMR_WIND_ENABLE_HYPRE:BOOL=ON  -DENABLE_HYPRE=ON -DAscent_DIR=/home/jkress/packages/visualizationPerformanceEvaluation/ascent/install-release/lib/cmake/ascent/ -DASCENT_DIR=/home/jkress/packages/visualizationPerformanceEvaluation/ascent/install-release/lib/cmake/ascent/ -DADIOS2_DIR=/home/jkress/packages/visualizationPerformanceEvaluation/adios2/install -DFides_DIR=/home/jkress/packages/visualizationPerformanceEvaluation/fides/install/lib/cmake/fides -DHYPRE_ROOT:PATH=/home/jkress/spack/opt/spack/linux-linuxmint20-haswell/gcc-9.3.0/hypre-2.20.0-hhiu3wzuyxp4keqxhyxotisx3nj6hv6z -DAMR_WIND_ENABLE_ADIOS2=ON -DAMR_WIND_ENABLE_FIDES=ON
