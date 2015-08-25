#include <iostream>

#ifndef NESTNODESYNAPSE_CLASS
#define NESTNODESYNAPSE_CLASS

typedef __int32 int32_t;
typedef __int64 int64_t;
typedef unsigned __int32 uint32_t;
typedef unsigned __int64 uint64_t;

struct Coords
{
  double x_;
  double y_;
  double z_;
};// source_neuron_coords;*/

class NESTNodeSynapse
{
private:
public:  
    NESTNodeSynapse();
    NESTNodeSynapse(const unsigned int& source_neuron_, const unsigned int& target_neuron_);
    ~NESTNodeSynapse();
    

    unsigned int source_neuron_;
    unsigned int target_neuron_;
    unsigned int node_id_;
    
    
    
    
    //Coords source_neuron_pos_;
    
    void set(const unsigned int& source_neuron_, const unsigned int& target_neuron_);
    
    void serialize(unsigned int* buf);
    void deserialize(unsigned int* buf);
    
    bool operator<(const NESTNodeSynapse& rhs) const;
};



#endif