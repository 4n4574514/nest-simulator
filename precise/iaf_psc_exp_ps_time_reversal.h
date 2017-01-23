/*
 *  iaf_psc_exp_ps_time_reversal.h
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


#ifndef IAF_PSC_EXP_PS_TIME_REVERSAL_H
#define IAF_PSC_EXP_PS_TIME_REVERSAL_H

#include "config.h"

#include "archiving_node.h"
#include "nest_types.h"
#include "event.h"
#include "ring_buffer.h"
#include "slice_ring_buffer.h"
#include "connection.h"
#include "universal_data_logger.h"
#include "stopwatch.h"
#include "arraydatum.h"

#include <vector>

/*BeginDocumentation
Name: iaf_psc_exp_ps_time_reversal - Leaky integrate-and-fire neuron
with exponential postsynaptic currents; canoncial implementation;
bisectioning method for approximation of threshold crossing.

Description:
iaf_psc_exp_ps_time_reversal is the "canonical" implementation of the leaky
integrate-and-fire model neuron with exponential postsynaptic currents
that uses the bisectioning method to approximate the timing of a threshold
crossing [1,2]. This is the most exact implementation available.

The canonical implementation handles neuronal dynamics in a locally
event-based manner with in coarse time grid defined by the minimum
delay in the network, see [1,2]. Incoming spikes are applied at the
precise moment of their arrival, while the precise time of outgoing
spikes is determined by bisectioning once a threshold crossing has
been detected. Return from refractoriness occurs precisely at spike
time plus refractory period.

This implementation is more complex than the plain iaf_psc_exp
neuron, but achieves much higher precision. In particular, it does not
suffer any binning of spike times to grid points. Depending on your
application, the canonical application with bisectioning may provide
superior overall performance given an accuracy goal; see [1,2] for
details. Subthreshold dynamics are integrated using exact integration
between events [3].

Parameters: 
  The following parameters can be set in the status dictionary.
  E_L           double - Resting membrane potential in mV.
  C_m           double - Specific capacitance of the membrane in pF/mum^2.
  tau_m         double - Membrane time constant in ms.
  tau_syn_ex    double - Excitatory synaptic time constant in ms.
  tau_syn_in    double - Inhibitory synaptic time constant in ms.
  t_ref         double - Duration of refractory period in ms.
  V_th          double - Spike threshold in mV.
  I_e           double - Constant input current in pA.
  V_min         double - Absolute lower value for the membrane potential.
  V_reset       double - Reset value for the membrane potential.

Remarks:
  Please note that this node is capable of sending precise spike times
  to target nodes (on-grid spike time plus offset). If this node is
  connected to a spike_detector, the property "precise_times" of the
  spike_detector has to be set to true in order to record the offsets
  in addition to the on-grid spike times.
  
References:
  [1] Morrison A, Straube S, Plesser HE & Diesmann M (2007) Exact subthreshold
      integration with continuous spike times in discrete time neural network
      simulations. Neural Comput 19, 47–79
  [2] Hanuschkin A, Kunkel S, Helias M, Morrison A and Diesmann M (2010) A
      general and efficient method for incorporating precise spike times in
      globally timedriven simulations. Front Neuroinform 4:113
  [3] Rotter S & Diesmann M (1999) Exact simulation of time-invariant linear
      systems with applications to neuronal modeling. Biol Cybern 81:381-402
        
Author: Kunkel

Sends: SpikeEvent

Receives: SpikeEvent, CurrentEvent, DataLoggingRequest

SeeAlso: iaf_psc_exp, iaf_psc_alpha_canon
*/ 

namespace nest
{

  /**
   * Leaky iaf neuron, exponential PSC synapses, canonical implementation.
   * @note Inherit privately from Node, so no classes can be derived
   * from this one.
   * @todo Implement current input in consistent way.
   */
  class iaf_psc_exp_ps_time_reversal : public Node
  {
    
    class Network;
    
  public:
    
    /** Basic constructor.
        This constructor should only be used by GenericModel to create 
        model prototype instances.
    */
    iaf_psc_exp_ps_time_reversal();
    
    /** Copy constructor.
	GenericModel::allocate_() uses the copy constructor to clone
	actual model instances from the prototype instance. 
	
	@note The copy constructor MUST NOT be used to create nodes based
	on nodes that have been placed in the network.
    */ 
    iaf_psc_exp_ps_time_reversal(const iaf_psc_exp_ps_time_reversal &);
    
    /**
     * Import sets of overloaded virtual functions.
     * We need to explicitly include sets of overloaded
     * virtual functions into the current scope.
     * According to the SUN C++ FAQ, this is the correct
     * way of doing things, although all other compilers
     * happily live without.
     */
    using Node::handles_test_event;
    using Node::handle;
    
    port send_test_event( Node&, rport, synindex, bool );
    
    void handle(SpikeEvent &);
    void handle(CurrentEvent &);
    void handle(DataLoggingRequest &);
    
    port handles_test_event(SpikeEvent &, port);
    port handles_test_event(CurrentEvent &, port);
    port handles_test_event(DataLoggingRequest &, port);
    
    bool is_off_grid() const  // uses off_grid events
    {
      return true;
    }
    
    void get_status(DictionaryDatum &) const;
    void set_status(const DictionaryDatum &);
    
  private:

    /** @name Interface functions
     * @note These functions are private, so that they can be accessed
     * only through a Node*. 
     */
    //@{
    void init_node_(const Node & proto);
    void init_state_(const Node & proto);
    void init_buffers_();
    void calibrate();

    /**
     * Time Evolution Operator.
     *
     * update() promotes the state of the neuron from origin+from to origin+to.
     * It does so in steps of the resolution h.  Within each step, time is
     * advanced from event to event, as retrieved from the spike queue.  
     *
     * Return from refractoriness is handled as a special event in the
     * queue, which is marked by a weight that is GSL_NAN.  This greatly simplifies
     * the code.
     *
     * For steps, during which no events occur, the precomputed propagator matrix
     * is used.  For other steps, the propagator matrix is computed as needed.
     *
     * While the neuron is refractory, membrane potential (y2_) is
     * clamped to U_reset_.
     */
    void update(Time const & origin, const long from, const long to);
    //@}
    
    // The next two classes need to be friends to access the State_ class/member
    friend class RecordablesMap<iaf_psc_exp_ps_time_reversal>;
    friend class UniversalDataLogger<iaf_psc_exp_ps_time_reversal>;
    
    void set_spiketime(Time const &);
    
    /**
     * Propagate neuron state.
     * Propagate the neuron's state by dt.
     * @param dt Interval over which to propagate
     */
    void propagate_(const double_t dt);
    
    /**
     * Emit a single spike caused by DC current in absence of spike input.
     * Emits a single spike and reset neuron given that the membrane
     * potential was below threshold at the beginning of a mini-timestep
     * and above afterwards.
     *
     * @param origin  Time stamp at beginning of slice
     * @param lag     Time step within slice
     * @param t0      Beginning of mini-timestep
     * @param dt      Duration of mini-timestep
     */
    void emit_spike_(const Time & origin, const long lag, 
		     const double_t t0, const double_t dt);
    
    /**
     * Emit a single spike at a precisely given time.
     *
     * @param origin        Time stamp at beginning of slice
     * @param lag           Time step within slice
     * @param spike_offset  Time offset for spike
     */
    void emit_instant_spike_(const Time & origin, const long lag, 
			     const double_t spike_offset);
    
    /**
     * Localize threshold crossing by bisectioning.
     * @param   double_t length of interval since previous event
     * @returns time from previous event to threshold crossing
     */
    double_t bisectioning_(const double_t dt) const;

    void spike_test_count_(const double_t);
    void spike_test_(const double_t);
    bool is_spike_(const double_t);

    // ---------------------------------------------------------------- 

    /** 
     * Independent parameters of the model. 
     */
    struct Parameters_
    {
      /** Membrane time constant in ms. */
      double_t tau_m_; 
      
      /** Time constant of exc. synaptic current in ms. */
      double_t tau_ex_;
      
      /** Time constant of inh. synaptic current in ms. */
      double_t tau_in_;
      
      /** Membrane capacitance in pF. */
      double_t c_m_;
      
      /** Refractory period in ms. */
      double_t t_ref_;
      
      /** Resting potential in mV. */
      double_t E_L_;
      
      /** External DC current [pA] */
      double_t I_e_;
      
      /** Threshold, RELATIVE TO RESTING POTENTAIL(!).
          I.e. the real threshold is U_th_ + E_L_. */
      double_t U_th_;
      
      /** Lower bound, RELATIVE TO RESTING POTENTAIL(!).
          I.e. the real lower bound is U_min_+E_L_. */
      double_t U_min_;
      
      /** Reset potential. 
	  At threshold crossing, the membrane potential is reset to this value. 
	  Relative to resting potential. */
      double_t U_reset_;

      double_t a1_;
      double_t a2_;
      double_t a3_;
      double_t a4_;

      double_t b1_;
      double_t b2_;
      double_t b3_;
      double_t b4_;
      double_t b5_;
      double_t b6_;
      double_t b7_;

      double_t c1_;
      double_t c2_;
      double_t c3_;
      double_t c4_;
      double_t c5_;
      double_t c6_;


      double_t d1_;
      double_t d2_;
      double_t d3_;

      
      Parameters_();  //!< Sets default parameter values
      void calc_const_spike_test_();

      void get(DictionaryDatum &) const;  //!< Store current values in dictionary
      double set(const DictionaryDatum &);  //!< Set values from dicitonary
    
    };
    
    // ---------------------------------------------------------------- 

    /**
     * State variables of the model.
     */
    struct State_
    {
      double_t y0_;  //!< External input current
      double_t y1_ex_;  //!< Exc. exponetial current
      double_t y1_in_;  //!< Inh. exponetial current
      double_t y2_;  //!< Membrane potential (relative to resting potential)
      
      bool is_refractory_;  //!< True while refractory
      long   last_spike_step_;  //!< Time stamp of most recent spike
      double_t last_spike_offset_;  //!< Offset of most recent spike

      long dhaene_quick1;
      long dhaene_quick2;
      long dhaene_tmax_lt_t1;
      long dhaene_max;
      long dhaene_det_spikes;

      long c0;
      long c1a;
      long c1b;
      long c2;
      long c3a;
      long c3b;
      long c4;
      long det_spikes;
      long state_space_test_spikes;

      State_();  //!< Default initialization
      
      void get(DictionaryDatum &, const Parameters_ &) const;
      void set(const DictionaryDatum &, const Parameters_ &, double delta_EL);





    };
    
    // ---------------------------------------------------------------- 

    /**
     * Buffers of the model.
     */
    struct Buffers_
    {
      Buffers_(iaf_psc_exp_ps_time_reversal &);
      Buffers_(const Buffers_ &, iaf_psc_exp_ps_time_reversal &);

      /**
       * Queue for incoming events.
       * @note Handles also pseudo-events marking return from refractoriness.
       */
      SliceRingBuffer events_;
      RingBuffer currents_; 
      
      //! Logger for all analog data
      UniversalDataLogger<iaf_psc_exp_ps_time_reversal> logger_;
    };

    // ---------------------------------------------------------------- 

    /**
     * Internal variables of the model.
     */
    struct Variables_
    { 
      double_t h_ms_;              //!< Time resolution [ms]
      long   refractory_steps_;  //!< Refractory time in steps
      double_t expm1_tau_m_;       //!< exp(-h/tau_m) - 1
      double_t expm1_tau_ex_;      //!< exp(-h/tau_ex) - 1
      double_t expm1_tau_in_;      //!< exp(-h/tau_in) - 1
      double_t P20_;               //!< Progagator matrix element, 2nd row
      double_t P21_in_;            //!< Progagator matrix element, 2nd row
      double_t P21_ex_;            //!< Progagator matrix element, 2nd row
      double_t y0_before_;         //!< y0_ at beginning of ministep
      double_t y1_ex_before_;      //!< y1_ at beginning of ministep
      double_t y1_in_before_;      //!< y1_ at beginning of ministep
      double_t y2_before_;         //!< y2_ at beginning of ministep
      double_t bisection_step;
	};
    
    // Access functions for UniversalDataLogger -------------------------------
    
    //! Read out the real membrane potential
    double_t get_V_m_() const { return S_.y2_ + P_.E_L_; }
    double_t get_I_syn_() const { return S_.y1_ex_ + S_.y1_in_; }
    double_t get_y1_ex_() const { return S_.y1_ex_; }
    double_t get_y1_in_() const { return S_.y1_in_; }
    double_t get_y0_() const {return S_.y0_;}

    
    // ---------------------------------------------------------------- 
    
    /**
     * @defgroup iaf_psc_exp_ps_time_reversal_data
     * Instances of private data structures for the different types
     * of data pertaining to the model.
     * @note The order of definitions is important for speed.
     * @{
     */   
    Parameters_ P_;
    State_      S_;
    Variables_  V_;
    Buffers_    B_;
    /** @} */
    
    //! Mapping of recordables names to access functions
    static RecordablesMap<iaf_psc_exp_ps_time_reversal> recordablesMap_;
  };
  
inline
port iaf_psc_exp_ps_time_reversal::send_test_event( Node& target, rport receptor_type, synindex, bool )
{
  SpikeEvent e;
  
  e.set_sender(*this);
  //c.check_event(e);
  return target.handles_test_event( e, receptor_type );
}

inline
port iaf_psc_exp_ps_time_reversal::handles_test_event(SpikeEvent &, port receptor_type)
{
  if (receptor_type != 0)
    throw UnknownReceptorType(receptor_type, get_name());
  return 0;
}

inline
port iaf_psc_exp_ps_time_reversal::handles_test_event(CurrentEvent &, port receptor_type)
{
  if (receptor_type != 0)
    throw UnknownReceptorType(receptor_type, get_name());
  return 0;
}

inline
port iaf_psc_exp_ps_time_reversal::handles_test_event(DataLoggingRequest & dlr, 
				    port receptor_type)
{
  if (receptor_type != 0)
    throw UnknownReceptorType(receptor_type, get_name());
  return B_.logger_.connect_logging_device(dlr, recordablesMap_);
}

inline
void iaf_psc_exp_ps_time_reversal::get_status(DictionaryDatum & d) const
{
  P_.get(d);
  S_.get(d, P_);
  (*d)[names::recordables] = recordablesMap_.get_list();

}

inline
void iaf_psc_exp_ps_time_reversal::set_status(const DictionaryDatum & d)
{
  Parameters_ ptmp = P_;  // temporary copy in case of errors
  double_t delta_EL = ptmp.set(d);            // throws if BadProperty
  State_ stmp = S_;       // temporary copy in case of errors
  stmp.set(d, ptmp, delta_EL);      // throws if BadProperty

  // if we get here, temporaries contain consistent set of properties
  P_ = ptmp;
  S_ = stmp;
}


// inline
// void iaf_psc_exp_ps_time_reversal::print_stopwatch()
// {
//   regime1.print("branch-1 (no-spike) took: ");
//   regime2.print("branch-2 (no-spike) took: ");
//   regime3.print("branch-3 (spike) took: ");

// }

inline
void nest::iaf_psc_exp_ps_time_reversal::spike_test_(const double_t t1)
{
  // we assume that P_.tau_ex_=P_.tau_in_
  double_t const I_0   = V_.y1_ex_before_ + V_.y1_in_before_;
  double_t const V_0   = V_.y2_before_;
  double_t const I_t1  = S_.y1_ex_ + S_.y1_in_;
  double_t const V_t1  = S_.y2_;
  double_t const tau   = P_.tau_ex_;
  double_t const tau_m = P_.tau_m_;
  double_t const I_x   = P_.I_e_;
  double_t const C_m   = P_.c_m_;
  double_t const V_th  = P_.U_th_;

  double_t const tauC_m = tau_m/C_m;

  double_t const Vdot_t1 = -V_t1/tau_m + (I_t1+I_x)/C_m;


  // iaflossless tests
  if ( Vdot_t1 < 0.0 )
  {
    double_t const Vdot_0  = -V_0/tau_m + (I_0+I_x)/C_m;

    if ( Vdot_0 > 0.0 )
    {
      if ( Vdot_0*t1 + V_0 >= V_th )
      {
        //if ( V_0 + Vdot_0 * (V_0 - V_t1 + Vdot_t1*t1) / (Vdot_t1-Vdot_0) >= V_th )
    	//{
    	  /* //final iaflossles test */
    	  /* double_t const V_th_bar = V_th - tauC_m*I_x;  */
    	  /* double_t const y        = V_th_bar/tauC_m / I_0; */

    	  /* if ( V_0 >= tau_m/(tau_m-tau) * (-tau/C_m * I_0 + V_th_bar * pow(y, (-tau/tau_m))) ) */
	  /*   S_.det_spikes++; */

  	  // D'Haene tests
  	  double_t const minus_taus = -tau_m*tau / (tau_m-tau);
  	  double_t const V_syn      = minus_taus / C_m * I_0;
  	  double_t const V_m        = V_0 - tauC_m*I_x - V_syn;
  	  double_t const quot       = -tau*V_m / (tau_m*V_syn);

  	  double_t const t_max = minus_taus * log(quot);

  	  double_t const expm1_tau_syn = numerics::expm1(-t_max/tau);
  	  double_t const expm1_tau_m   = numerics::expm1(-t_max/tau_m);
      
  	  double_t const P20 = -tau_m*expm1_tau_m / C_m;
  	  double_t const P21 = minus_taus / C_m * (expm1_tau_syn-expm1_tau_m);
      
  	  if ( (P20*I_x + P21*I_0 + expm1_tau_m*V_0 + V_0) >= V_th )
  	    S_.dhaene_det_spikes++;

	  //}
      }
    }
  }
}

//time-reversal state space analysis test 
//looks for the no-spike region first
//the state space test takes argument dt and
//returns true, spike: if (V(t_{right}) > V_(\theta));
//returns false: ( (V(t_{right} < V_(\theta) or initial conditions in no-spike region);
//returns true, spike: missed spike excursion, compute t_{max} = dt and find point of
//threshold crossing t_{\theta} using emit_spike_.

inline
bool iaf_psc_exp_ps_time_reversal::is_spike_(double_t dt)
{
  double_t const I_0   = V_.y1_ex_before_ + V_.y1_in_before_;
  double_t const V_0   = V_.y2_before_; 
  const double_t exp_tau_s = numerics::expm1(dt/P_.tau_ex_) ; //inequalities are adjusted such that backward propagation (negative time) is already accounted for here
  const double_t exp_tau_m  = numerics::expm1(dt/P_.tau_m_) ; 
  const double_t exp_tau_m_s = numerics::expm1(dt/P_.tau_m_ - dt/P_.tau_ex_);
  
  //pre-compute g
  double_t g = ((P_.a1_ * I_0 * exp_tau_m_s + exp_tau_m * (P_.a3_ - P_.I_e_ * P_.a2_) + P_.a3_)/P_.a4_) ; 

    //no-spike
    // intersecting line
  if((V_0 <= (((I_0 + P_.I_e_)*(P_.b1_ * exp_tau_m + P_.b2_* exp_tau_s) + P_.b5_*(exp_tau_m - exp_tau_s))/( P_.b7_ * exp_tau_s)))
    //continuation line       
      &&  (V_0 < g))     
    {
      return false;
    }
  
    //spike
  else if (V_0 >= g )  
    {
      return true;
    }
  //no-spike
  else if(V_0 < (P_.c1_ * P_.I_e_ + P_.c2_ * I_0 + P_.c3_* pow(I_0, P_.c4_) * pow((P_.c5_ - P_.I_e_), P_.c6_)))
    { 
      return false;
    }
  else
  //spike
    {
      V_.bisection_step = (P_.a1_ / P_.tau_m_ * P_.tau_ex_ ) * log ( P_.b1_ * I_0 / (P_.a2_ * P_.I_e_ - P_.a1_ * I_0 - P_.a4_ * V_0 ) );
      return true;
    }

}
  
} // namespace

#endif // IAF_PSC_EXP_PS_TIME_REVERSAL_H


