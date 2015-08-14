import sys
sys.path.append('/home/deger/nest/nest-git-mdeger/install/lib/python2.7/site-packages')
sys.path.reverse()

import nest
import nest.raster_plot
import numpy as np
import pylab

Dt = 1.
nsteps = 100
w_0 = 100.

nest.ResetKernel()

nrn_pre = nest.Create('parrot_neuron')
nrn_post1 = nest.Create('iaf_psc_delta')
nrn_post2 = nest.Create('pp_psc_delta')

nest.Connect( nrn_pre, nrn_post1 + nrn_post2, \
    syn_spec={'model':'stdp_synapse', 'weight': w_0} )
conn1 = nest.GetConnections(nrn_pre, nrn_post1)
conn2 = nest.GetConnections(nrn_pre, nrn_post2)

sg_pre = nest.Create('spike_generator')
nest.SetStatus( sg_pre, {'spike_times': np.arange(Dt, nsteps*Dt, 10.*Dt)})
nest.Connect( sg_pre, nrn_pre )

mm = nest.Create('multimeter')
nest.SetStatus(mm, {'record_from':['V_m']})
nest.Connect( mm, nrn_post1+nrn_post2 )

sd = nest.Create('spike_detector')
nest.Connect( nrn_pre + nrn_post1 + nrn_post2, sd )

t = []
w1 = []
w2 = []
t.append( 0. )
w1.append( nest.GetStatus(conn1, keys=['weight'])[0][0] )
w2.append( nest.GetStatus(conn2, keys=['weight'])[0][0] )

for i in xrange(nsteps):
    nest.Simulate( Dt )
    t.append( i*Dt )
    w1.append( nest.GetStatus(conn1, keys=['weight'])[0][0] )
    w2.append( nest.GetStatus(conn2, keys=['weight'])[0][0] )

pylab.figure(1)
pylab.plot(t, w1, 'g', label='iaf_psc_delta, '+str(nrn_post1[0]))
pylab.plot(t, w2, 'r', label='pp_psc_delta, '+str(nrn_post2[0]))
pylab.xlabel('time [ms]')
pylab.ylabel('weight [mV]')
pylab.legend(loc='best')
ylims = pylab.ylim()
pylab.ylim(ylims[0]-5, ylims[1]+5)
pylab.savefig('test_pp_psc_delta_stdp_fig1.pdf')

nest.raster_plot.from_device(sd)
ylims = pylab.ylim()
pylab.ylim(ylims[0]-.5, ylims[1]+.5)
pylab.show()
pylab.savefig('test_pp_psc_delta_stdp_fig1.pdf')


print 'archiver-lengths are different:'
for nrn in [nrn_post1, nrn_post2]:
    print nest.GetStatus(nrn, keys=['model', 'archiver_length'])[0]



