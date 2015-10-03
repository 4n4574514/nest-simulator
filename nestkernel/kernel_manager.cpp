/*
 *  kernel_manager.cpp
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

#include "kernel_manager.h"

nest::KernelManager* nest::KernelManager::kernel_manager_instance_ = 0;

void
nest::KernelManager::create_kernel_manager()
{
#pragma omp critical( create_kernel_manager )
  {
    if ( kernel_manager_instance_ == 0 )
    {
      kernel_manager_instance_ = new KernelManager();
      assert( kernel_manager_instance_ );
    }
  }
}

void
nest::KernelManager::destroy_kernel_manager()
{
  delete kernel_manager_instance_;
}

nest::KernelManager::KernelManager()
  : initialized_( false )
{
}

nest::KernelManager::~KernelManager()
{
}

void
nest::KernelManager::init()
{
  logging_manager.init();
  mpi_manager.init();
  vp_manager.init();
  io_manager.init();
  connection_builder_manager.init();
  event_delivery_manager.init();
  simulation_manager.init();
  modelrange_manager.init();

  initialized_ = true;
}

void
nest::KernelManager::reset()
{
  initialized_ = false;

  logging_manager.reset();
  mpi_manager.reset();
  vp_manager.reset();
  io_manager.reset();
  connection_builder_manager.reset();
  event_delivery_manager.reset();
  simulation_manager.reset();
  modelrange_manager.reset();
}

void
nest::KernelManager::set_status( const DictionaryDatum& dict )
{
  logging_manager.set_status( dict );
  mpi_manager.set_status( dict );
  vp_manager.set_status( dict );
  io_manager.set_status( dict );
  connection_builder_manager.set_status( dict );
  event_delivery_manager.set_status( dict );
  simulation_manager.set_status( dict );
  modelrange_manager.set_status( dict );
}

void
nest::KernelManager::get_status( DictionaryDatum& dict )
{
  logging_manager.get_status( dict );
  mpi_manager.get_status( dict );
  vp_manager.get_status( dict );
  io_manager.get_status( dict );
  connection_builder_manager.get_status( dict );
  event_delivery_manager.get_status( dict );
  simulation_manager.get_status( dict );
  modelrange_manager.get_status( dict );
}
