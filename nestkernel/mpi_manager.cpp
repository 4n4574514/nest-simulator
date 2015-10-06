/*
 *  mpi_manager.cpp
 *
 *  This file is part of NEST.
 *
 *  Copyright (C) 2004 The NEST Initiative
 *
 *  NEST is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  NEST is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with NEST.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "mpi_manager.h"
#include "communicator.h"
#include "network.h"
#include "dictutils.h"
#include "kernel_manager.h"
#include "logging.h"

#ifdef HAVE_MPI
#include <mpi.h>

extern MPI_Comm comm; // for now---should be moved from communicator.cpp
#endif

nest::MPIManager::MPIManager()
  : num_processes_( 1 )
  , rank_( 0 )
  , n_rec_procs_( 0 )
  , n_sim_procs_( 0 )
{
}

void
nest::MPIManager::init_mpi( int* argc, char** argv[] )
{
#ifdef HAVE_MPI
  int init;
  MPI_Initialized( &init );

  int provided_thread_level;
  if ( init == 0 )
  {

#ifdef HAVE_MUSIC
    music_setup = new MUSIC::Setup( *argc, *argv, MPI_THREAD_FUNNELED, &provided_thread_level );
    // get a communicator from MUSIC
    comm = music_setup->communicator();
#else  /* #ifdef HAVE_MUSIC */
    MPI_Init_thread( argc, argv, MPI_THREAD_FUNNELED, &provided_thread_level );
    comm = MPI_COMM_WORLD;
#endif /* #ifdef HAVE_MUSIC */
  }

  MPI_Comm_size( comm, &num_processes_ );
  MPI_Comm_rank( comm, &rank_ );
  Communicator::init();
#endif /* #ifdef HAVE_MPI */
}

void
nest::MPIManager::init()
{
  n_sim_procs_ = num_processes_ - n_rec_procs_;
}

void
nest::MPIManager::reset()
{
}

void
nest::MPIManager::set_status( const DictionaryDatum& d )
{
}

void
nest::MPIManager::get_status( DictionaryDatum& d )
{
  def< long >( d, "num_processes", num_processes_ );
}

void
nest::MPIManager::set_num_rec_processes( int nrp, bool called_by_reset )
{
  if ( Network::get_network().size() > 1 and not called_by_reset )
    throw KernelException(
      "Global spike detection mode must be enabled before nodes are created." );
  if ( nrp >= num_processes_ )
    throw KernelException(
      "Number of processes used for recording must be smaller than total number of processes." );
  n_rec_procs_ = nrp;
  n_sim_procs_ = num_processes_ - n_rec_procs_;
  Network::get_network().create_rngs_( true );
  if ( nrp > 0 )
  {
    std::string msg = String::compose(
      "Entering global spike detection mode with %1 recording MPI processes and %2 simulating MPI "
      "processes.",
      n_rec_procs_,
      n_sim_procs_ );
    LOG( M_INFO, "Network::set_num_rec_processes", msg );
  }
}
