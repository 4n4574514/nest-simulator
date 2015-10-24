/*
 *  network.h
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

#ifndef NETWORK_H
#define NETWORK_H

#include <dirent.h>
#include <errno.h>

#include <vector>
#include <string>
#include <typeinfo>
#include <ostream>
#include <cmath>

#include "config.h"
#include "nest_types.h"
#include "nest_time.h"
#include "model.h"
#include "dict.h"
#include "dictdatum.h"


#ifdef M_ERROR
#undef M_ERROR
#endif

#ifdef _OPENMP
#include <omp.h>
#endif

#ifdef HAVE_MUSIC
#include "music_event_handler.h"
#endif

/**
 * @file network.h
 * Declarations for class Network.
 */
class SLIInterpreter;

namespace nest
{

class VPManager;
class NodeManager;

/**
 * @defgroup network Network access and administration
 * @brief Network administration and scheduling.
 * This module contains all classes which are involved in the
 * administration of the Network and the scheduling during
 * simulation.
 */

/**
 * Main administrative interface to the network.
 * Class Network is responsible for
 * -# Administration of Model objects.
 * -# Administration of network Nodes.
 * -# Administration of the simulation time.
 * -# Update and scheduling during simulation.
 * -# Memory cleanup at exit.
 *
 * @see Node
 * @see Model
 * @ingroup user_interface
 * @ingroup network
 */

class Network
{
  friend class VPManager;
  friend class SimulationManager;
  friend class ConnectionBuilderManager;
  friend class EventDeliveryManager;
  friend class MPIManager;
  friend class NodeManager;

private:
  Network( SLIInterpreter& );
  static Network* network_instance_;
  static bool created_network_instance_;

  Network( Network const& );        // Don't Implement
  void operator=( Network const& ); // Don't Implement

public:
  /**
   * Create/destroy and access the Network singleton.
   */
  static void create_network( SLIInterpreter& );
  static void destroy_network();
  static Network& get_network();

  ~Network();

  /**
   * Reset number of threads to one, reset device prefix to the
   * empty string and call reset().
   */
  void reset_kernel();

  /**
   * @defgroup net_access Network access
   * Functions to access network nodes.
   */


  /**
   * Get properties of a node. The specified node must exist.
   * @throws nest::UnknownNode       Target does not exist in the network.
   */
  DictionaryDatum get_status( index );

  /**
   * Execute a SLI command in the neuron's namespace.
   */
  int execute_sli_protected( DictionaryDatum, Name );

#ifdef HAVE_MUSIC
public:
  /**
   * Register a MUSIC input port (portname) with the port list.
   * This will increment the counter of the respective entry in the
   * music_in_portlist.
   */
  void register_music_in_port( std::string portname );

  /**
   * Unregister a MUSIC input port (portname) from the port list.
   * This will decrement the counter of the respective entry in the
   * music_in_portlist and remove the entry if the counter is 0
   * after decrementing it.
   */
  void unregister_music_in_port( std::string portname );

  /**
   * Register a node (of type music_input_proxy) with a given MUSIC
   * port (portname) and a specific channel. The proxy will be
   * notified, if a MUSIC event is being received on the respective
   * channel and port.
   */
  void register_music_event_in_proxy( std::string portname, int channel, nest::Node* mp );

  /**
   * Set the acceptable latency (latency) for a music input port (portname).
   */
  void set_music_in_port_acceptable_latency( std::string portname, double_t latency );
  void set_music_in_port_max_buffered( std::string portname, int_t maxbuffered );
  /**
   * Data structure to hold variables and parameters associated with a port.
   */
  struct MusicPortData
  {
    MusicPortData( size_t n, double_t latency, int_t m )
      : n_input_proxies( n )
      , acceptable_latency( latency )
      , max_buffered( m )
    {
    }
    MusicPortData()
    {
    }
    size_t n_input_proxies; // Counter for number of music_input proxies
                            // connected to this port
    double_t acceptable_latency;
    int_t max_buffered;
  };

  /**
   * The mapping between MUSIC input ports identified by portname
   * and the corresponding port variables and parameters.
   * @see register_music_in_port()
   * @see unregister_music_in_port()
   */
  std::map< std::string, MusicPortData > music_in_portlist_;

  /**
   * The mapping between MUSIC input ports identified by portname
   * and the corresponding MUSIC event handler.
   */
  std::map< std::string, MusicEventHandler > music_in_portmap_;

  /**
   * Publish all MUSIC input ports that were registered using
   * Network::register_music_event_in_proxy().
   */
  void publish_music_in_ports_();

  /**
   * Call update() for each of the registered MUSIC event handlers
   * to deliver all queued events to the target music_in_proxies.
   */
  void update_music_event_handlers_( Time const&, const long_t, const long_t );
#endif


private:
  SLIInterpreter& interpreter_;
};

inline Network&
Network::get_network()
{
  assert( created_network_instance_ );
  return *network_instance_;
}

/****** former Scheduler functions ******/

} // namespace

#endif
