
#ifdef IS_BLUEGENE_Q
#include <spi/include/kernel/memory.h>
#endif

/**
 * Memory predictor for NEST 
 * included is a easy mem model of NEST based on synapse and neuron number
 * 
 * 
 * 
 */

typedef __int32 int32_t;
typedef __int64 int64_t;
typedef unsigned __int32 uint32_t;
typedef unsigned __int64 uint64_t;

class H5SynMEMPredictor
{
public:
  H5SynMEMPredictor();
  ~H5SynMEMPredictor() {};
  
  
  void updateMEM();
  
  int preNESTCreate(const uint32_t& non);
  int preNESTConnect(const uint64_t& nos);
  void predictBestLoadNos(uint64_t& nos);
  
private:
  
  uint64_t measured_mem_free_begin;
  
  uint64_t measured_mem_free;
  uint64_t predicted_mem_used;
  
  uint32_t number_of_neurons;
  uint64_t number_of_synapses;
  
  uint64_t max_nos;
  
};