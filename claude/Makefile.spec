TUNE=base
LABEL=none
NUMBER=767
NAME=nest_r
SOURCES= nest-simulator/nest/main.cpp nest-simulator/models/models.cpp \
	 nest-simulator/models/modelsmodule.cpp \
	 nest-simulator/nest/neststartup.cpp \
	 nest-simulator/libnestutil/iaf_propagator.cpp \
	 nest-simulator/libnestutil/logging_event.cpp \
	 nest-simulator/libnestutil/numerics.cpp \
	 nest-simulator/libnestutil/stopwatch.cpp \
	 nest-simulator/models/ac_generator.cpp \
	 nest-simulator/models/aeif_cond_alpha.cpp \
	 nest-simulator/models/aeif_cond_alpha_astro.cpp \
	 nest-simulator/models/aeif_cond_alpha_multisynapse.cpp \
	 nest-simulator/models/aeif_cond_beta_multisynapse.cpp \
	 nest-simulator/models/aeif_cond_exp.cpp \
	 nest-simulator/models/aeif_psc_alpha.cpp \
	 nest-simulator/models/aeif_psc_delta.cpp \
	 nest-simulator/models/aeif_psc_delta_clopath.cpp \
	 nest-simulator/models/aeif_psc_exp.cpp \
	 nest-simulator/models/amat2_psc_exp.cpp \
	 nest-simulator/models/astrocyte_lr_1994.cpp \
	 nest-simulator/models/bernoulli_synapse.cpp \
	 nest-simulator/models/clopath_synapse.cpp \
	 nest-simulator/models/cm_compartmentcurrents.cpp \
	 nest-simulator/models/cm_default.cpp nest-simulator/models/cm_tree.cpp \
	 nest-simulator/models/cont_delay_synapse.cpp \
	 nest-simulator/models/correlation_detector.cpp \
	 nest-simulator/models/correlomatrix_detector.cpp \
	 nest-simulator/models/correlospinmatrix_detector.cpp \
	 nest-simulator/models/dc_generator.cpp \
	 nest-simulator/models/diffusion_connection.cpp \
	 nest-simulator/models/eprop_iaf_adapt_bsshslm_2020.cpp \
	 nest-simulator/models/eprop_iaf_bsshslm_2020.cpp \
	 nest-simulator/models/eprop_learning_signal_connection_bsshslm_2020.cpp \
	 nest-simulator/models/eprop_readout_bsshslm_2020.cpp \
	 nest-simulator/models/eprop_synapse_bsshslm_2020.cpp \
	 nest-simulator/models/erfc_neuron.cpp \
	 nest-simulator/models/gamma_sup_generator.cpp \
	 nest-simulator/models/gap_junction.cpp \
	 nest-simulator/models/gauss_rate.cpp \
	 nest-simulator/models/gif_cond_exp.cpp \
	 nest-simulator/models/gif_cond_exp_multisynapse.cpp \
	 nest-simulator/models/gif_pop_psc_exp.cpp \
	 nest-simulator/models/gif_psc_exp.cpp \
	 nest-simulator/models/gif_psc_exp_multisynapse.cpp \
	 nest-simulator/models/ginzburg_neuron.cpp \
	 nest-simulator/models/glif_cond.cpp nest-simulator/models/glif_psc.cpp \
	 nest-simulator/models/glif_psc_double_alpha.cpp \
	 nest-simulator/models/hh_cond_beta_gap_traub.cpp \
	 nest-simulator/models/hh_cond_exp_traub.cpp \
	 nest-simulator/models/hh_psc_alpha.cpp \
	 nest-simulator/models/hh_psc_alpha_clopath.cpp \
	 nest-simulator/models/hh_psc_alpha_gap.cpp \
	 nest-simulator/models/ht_neuron.cpp nest-simulator/models/ht_synapse.cpp \
	 nest-simulator/models/iaf_chs_2007.cpp \
	 nest-simulator/models/iaf_chxk_2008.cpp \
	 nest-simulator/models/iaf_cond_alpha.cpp \
	 nest-simulator/models/iaf_cond_alpha_mc.cpp \
	 nest-simulator/models/iaf_cond_beta.cpp \
	 nest-simulator/models/iaf_cond_exp.cpp \
	 nest-simulator/models/iaf_cond_exp_sfa_rr.cpp \
	 nest-simulator/models/iaf_psc_alpha.cpp \
	 nest-simulator/models/iaf_psc_alpha_multisynapse.cpp \
	 nest-simulator/models/iaf_psc_alpha_ps.cpp \
	 nest-simulator/models/iaf_psc_delta.cpp \
	 nest-simulator/models/iaf_psc_delta_ps.cpp \
	 nest-simulator/models/iaf_psc_exp.cpp \
	 nest-simulator/models/iaf_psc_exp_htum.cpp \
	 nest-simulator/models/iaf_psc_exp_multisynapse.cpp \
	 nest-simulator/models/iaf_psc_exp_ps.cpp \
	 nest-simulator/models/iaf_psc_exp_ps_lossless.cpp \
	 nest-simulator/models/iaf_tum_2000.cpp \
	 nest-simulator/models/ignore_and_fire.cpp \
	 nest-simulator/models/inhomogeneous_poisson_generator.cpp \
	 nest-simulator/models/izhikevich.cpp \
	 nest-simulator/models/jonke_synapse.cpp \
	 nest-simulator/models/lin_rate.cpp \
	 nest-simulator/models/mat2_psc_exp.cpp \
	 nest-simulator/models/mcculloch_pitts_neuron.cpp \
	 nest-simulator/models/mip_generator.cpp \
	 nest-simulator/models/modelsmodule.cpp \
	 nest-simulator/models/multimeter.cpp \
	 nest-simulator/models/music_cont_in_proxy.cpp \
	 nest-simulator/models/music_cont_out_proxy.cpp \
	 nest-simulator/models/music_event_in_proxy.cpp \
	 nest-simulator/models/music_event_out_proxy.cpp \
	 nest-simulator/models/music_message_in_proxy.cpp \
	 nest-simulator/models/music_rate_in_proxy.cpp \
	 nest-simulator/models/music_rate_out_proxy.cpp \
	 nest-simulator/models/noise_generator.cpp \
	 nest-simulator/models/parrot_neuron.cpp \
	 nest-simulator/models/parrot_neuron_ps.cpp \
	 nest-simulator/models/poisson_generator.cpp \
	 nest-simulator/models/poisson_generator_ps.cpp \
	 nest-simulator/models/pp_cond_exp_mc_urbanczik.cpp \
	 nest-simulator/models/pp_psc_delta.cpp \
	 nest-simulator/models/ppd_sup_generator.cpp \
	 nest-simulator/models/pulsepacket_generator.cpp \
	 nest-simulator/models/quantal_stp_synapse.cpp \
	 nest-simulator/models/rate_connection_delayed.cpp \
	 nest-simulator/models/rate_connection_instantaneous.cpp \
	 nest-simulator/models/sic_connection.cpp \
	 nest-simulator/models/siegert_neuron.cpp \
	 nest-simulator/models/sigmoid_rate.cpp \
	 nest-simulator/models/sigmoid_rate_gg_1998.cpp \
	 nest-simulator/models/sinusoidal_gamma_generator.cpp \
	 nest-simulator/models/sinusoidal_poisson_generator.cpp \
	 nest-simulator/models/spike_dilutor.cpp \
	 nest-simulator/models/spike_generator.cpp \
	 nest-simulator/models/spike_recorder.cpp \
	 nest-simulator/models/spike_train_injector.cpp \
	 nest-simulator/models/spin_detector.cpp \
	 nest-simulator/models/static_synapse.cpp \
	 nest-simulator/models/static_synapse_hom_w.cpp \
	 nest-simulator/models/stdp_dopamine_synapse.cpp \
	 nest-simulator/models/stdp_facetshw_synapse_hom.cpp \
	 nest-simulator/models/stdp_nn_pre_centered_synapse.cpp \
	 nest-simulator/models/stdp_nn_restr_synapse.cpp \
	 nest-simulator/models/stdp_nn_symm_synapse.cpp \
	 nest-simulator/models/stdp_pl_synapse_hom.cpp \
	 nest-simulator/models/stdp_synapse.cpp \
	 nest-simulator/models/stdp_synapse_hom.cpp \
	 nest-simulator/models/stdp_triplet_synapse.cpp \
	 nest-simulator/models/step_current_generator.cpp \
	 nest-simulator/models/step_rate_generator.cpp \
	 nest-simulator/models/tanh_rate.cpp \
	 nest-simulator/models/threshold_lin_rate.cpp \
	 nest-simulator/models/tsodyks2_synapse.cpp \
	 nest-simulator/models/tsodyks_synapse.cpp \
	 nest-simulator/models/tsodyks_synapse_hom.cpp \
	 nest-simulator/models/urbanczik_synapse.cpp \
	 nest-simulator/models/vogels_sprekeler_synapse.cpp \
	 nest-simulator/models/volume_transmitter.cpp \
	 nest-simulator/models/weight_optimizer.cpp \
	 nest-simulator/models/weight_recorder.cpp \
	 nest-simulator/nestkernel/archiving_node.cpp \
	 nest-simulator/nestkernel/buffer_resize_log.cpp \
	 nest-simulator/nestkernel/clopath_archiving_node.cpp \
	 nest-simulator/nestkernel/common_synapse_properties.cpp \
	 nest-simulator/nestkernel/conn_builder.cpp \
	 nest-simulator/nestkernel/conn_builder_conngen.cpp \
	 nest-simulator/nestkernel/conn_parameter.cpp \
	 nest-simulator/nestkernel/connection_creator.cpp \
	 nest-simulator/nestkernel/connection_id.cpp \
	 nest-simulator/nestkernel/connection_manager.cpp \
	 nest-simulator/nestkernel/connector_model.cpp \
	 nest-simulator/nestkernel/delay_checker.cpp \
	 nest-simulator/nestkernel/deprecation_warning.cpp \
	 nest-simulator/nestkernel/device.cpp \
	 nest-simulator/nestkernel/dynamicloader.cpp \
	 nest-simulator/nestkernel/eprop_archiving_node.cpp \
	 nest-simulator/nestkernel/event.cpp \
	 nest-simulator/nestkernel/event_delivery_manager.cpp \
	 nest-simulator/nestkernel/exceptions.cpp \
	 nest-simulator/nestkernel/growth_curve.cpp \
	 nest-simulator/nestkernel/histentry.cpp \
	 nest-simulator/nestkernel/io_manager.cpp \
	 nest-simulator/nestkernel/kernel_manager.cpp \
	 nest-simulator/nestkernel/layer.cpp \
	 nest-simulator/nestkernel/logging_manager.cpp \
	 nest-simulator/nestkernel/mask.cpp nest-simulator/nestkernel/model.cpp \
	 nest-simulator/nestkernel/model_manager.cpp \
	 nest-simulator/nestkernel/modelrange.cpp \
	 nest-simulator/nestkernel/modelrange_manager.cpp \
	 nest-simulator/nestkernel/module_manager.cpp \
	 nest-simulator/nestkernel/mpi_manager.cpp \
	 nest-simulator/nestkernel/music_event_handler.cpp \
	 nest-simulator/nestkernel/music_manager.cpp \
	 nest-simulator/nestkernel/music_rate_in_handler.cpp \
	 nest-simulator/nestkernel/nest.cpp \
	 nest-simulator/nestkernel/nest_datums.cpp \
	 nest-simulator/nestkernel/nest_names.cpp \
	 nest-simulator/nestkernel/nest_time.cpp \
	 nest-simulator/nestkernel/nest_timeconverter.cpp \
	 nest-simulator/nestkernel/nestmodule.cpp \
	 nest-simulator/nestkernel/node.cpp \
	 nest-simulator/nestkernel/node_collection.cpp \
	 nest-simulator/nestkernel/node_manager.cpp \
	 nest-simulator/nestkernel/parameter.cpp \
	 nest-simulator/nestkernel/per_thread_bool_indicator.cpp \
	 nest-simulator/nestkernel/proxynode.cpp \
	 nest-simulator/nestkernel/random_manager.cpp \
	 nest-simulator/nestkernel/recording_backend.cpp \
	 nest-simulator/nestkernel/recording_backend_ascii.cpp \
	 nest-simulator/nestkernel/recording_backend_memory.cpp \
	 nest-simulator/nestkernel/recording_backend_screen.cpp \
	 nest-simulator/nestkernel/recording_device.cpp \
	 nest-simulator/nestkernel/ring_buffer.cpp \
	 nest-simulator/nestkernel/send_buffer_position.cpp \
	 nest-simulator/nestkernel/simulation_manager.cpp \
	 nest-simulator/nestkernel/slice_ring_buffer.cpp \
	 nest-simulator/nestkernel/sonata_connector.cpp \
	 nest-simulator/nestkernel/source_table.cpp \
	 nest-simulator/nestkernel/sp_manager.cpp \
	 nest-simulator/nestkernel/sparse_node_array.cpp \
	 nest-simulator/nestkernel/spatial.cpp \
	 nest-simulator/nestkernel/spikecounter.cpp \
	 nest-simulator/nestkernel/stimulation_device.cpp \
	 nest-simulator/nestkernel/structural_plasticity_node.cpp \
	 nest-simulator/nestkernel/synaptic_element.cpp \
	 nest-simulator/nestkernel/target_table.cpp \
	 nest-simulator/nestkernel/target_table_devices.cpp \
	 nest-simulator/nestkernel/vp_manager.cpp \
	 nest-simulator/sli/allocator.cpp nest-simulator/sli/slinames.cpp \
	 nest-simulator/sli/arraydatum.cc nest-simulator/sli/booldatum.cc \
	 nest-simulator/sli/charcode.cc nest-simulator/sli/datum.cc \
	 nest-simulator/sli/dict.cc nest-simulator/sli/dictstack.cc \
	 nest-simulator/sli/dictutils.cc nest-simulator/sli/doubledatum.cc \
	 nest-simulator/sli/functiondatum.cc nest-simulator/sli/integerdatum.cc \
	 nest-simulator/sli/interpret.cc nest-simulator/sli/literaldatum.cc \
	 nest-simulator/sli/name.cc nest-simulator/sli/namedatum.cc \
	 nest-simulator/sli/oosupport.cc nest-simulator/sli/parser.cc \
	 nest-simulator/sli/scanner.cc nest-simulator/sli/sli_io.cc \
	 nest-simulator/sli/sliactions.cc nest-simulator/sli/sliarray.cc \
	 nest-simulator/sli/slibuiltins.cc nest-simulator/sli/slicontrol.cc \
	 nest-simulator/sli/slidata.cc nest-simulator/sli/slidict.cc \
	 nest-simulator/sli/sliexceptions.cc nest-simulator/sli/sligraphics.cc \
	 nest-simulator/sli/slimath.cc nest-simulator/sli/slimodule.cc \
	 nest-simulator/sli/sliregexp.cc nest-simulator/sli/slistack.cc \
	 nest-simulator/sli/slistartup.cc nest-simulator/sli/slitype.cc \
	 nest-simulator/sli/slitypecheck.cc \
	 nest-simulator/sli/specialfunctionsmodule.cc \
	 nest-simulator/sli/stringdatum.cc nest-simulator/sli/symboldatum.cc \
	 nest-simulator/sli/tarrayobj.cc nest-simulator/sli/token.cc \
	 nest-simulator/sli/tokenarray.cc nest-simulator/sli/tokenstack.cc \
	 nest-simulator/sli/tokenutils.cc nest-simulator/sli/triedatum.cc \
	 nest-simulator/sli/typechk.cc nest-simulator/sli/utils.cc \
	 nest-simulator/rxspencer/regcomp.cc nest-simulator/rxspencer/regerror.cc \
	 nest-simulator/rxspencer/regexec.cc nest-simulator/rxspencer/regfree.cc \
	 nest-simulator/sli/fdstream.cc nest-simulator/sli/filesystem.cc
EXEBASE=nest_r
NEED_MATH=
BENCHLANG=CXX

BENCH_FLAGS      = -DSPEC_AUTO_BYTEORDER=0x12345678 -DSPEC_AUTO_SUPPRESS_THREADING -DDISABLE_TIMING -Inest-simulator -Inest-simulator/libnestutil -Inest-simulator/sli -Inest-simulator/rxspencer -Inest-simulator/thirdparty -Inest-simulator/nestkernel -Inest-simulator/models -Ispecrand-distributions
CC               = $(LLVM_BIN_DIR)/clang -g  -flto=thin
CC_VERSION_OPTION = --version
CLD              = $(LLVM_BIN_DIR)/clang 
COPTIMIZE        = -O3 -mcpu=native -ffast-math
CXX              = $(LLVM_BIN_DIR)/clang++ -std=c++03 -g  -flto=thin
CXXOPTIMIZE      = -O3 -mcpu=native -ffast-math
CXX_VERSION_OPTION = --version
EXTRA_CFLAGS     = 
EXTRA_CXXFLAGS   = -std=gnu++17 -U__cplusplus -D__cplusplus=201703L -D_GLIBCXX_USE_CXX11_ABI=1 
EXTRA_CXXOPTIMIZE = 
EXTRA_FFLAGS     = 
EXTRA_FOPTIMIZE  = 
EXTRA_PORTABILITY = -DSPEC_LP64 -Wno-int-conversion -Wno-implicit-function-declaration -Wno-implicit-int
FC               = $(LLVM_BIN_DIR)/flang -g -Mallocatable=03  -flto=thin
FC_VERSION_OPTION = --version
FLD              = $(LLVM_BIN_DIR)/flang 
FOPTIMIZE        = -O3 -mcpu=native -ffast-math
LDCFLAGS         = 
LDCXXFLAGS       = 
LDFFLAGS         = 
LDFLAGS          = -v  -Wl,--build-id -fuse-ld=lld -flto=thin -Wl,-mllvm,-thinlto-split=true -Wl,-mllvm,-thinlto-split-partitions=16 -Wl,-mllvm,-parallel-cloneModule=false 
LLVM_BIN_DIR     = /data2/jn/project/oe_llvm/llvm-project/thinlto-split/bin
LLVM_INCLUDE_DIR = /data2/jn/project/oe_llvm/llvm-project/thinlto-split/include
LLVM_LIB_DIR     = /data2/jn/project/oe_llvm/llvm-project/thinlto-split/lib
MATHLIBOPT       = -lm
OPTIMIZATION_CXXLIBS = 
OPTIMIZATION_FLIBS = 
OS               = unix
PORTABILITY      = -fno-finite-math-only
PORTABILITY_LIBS = -lstdc++fs
SPECLANG         = %{llvm_dir}/bin/
absolutely_no_locking = 0
abstol           = 
action           = validate
allow_label_override = 0
backup_config    = 0
baseexe          = nest_r
basepeak         = 1
bench_post_setup = sync
benchdir         = benchspec
benchmark        = 767.nest_r
binary           = 
bindir           = exe
builddir         = build
bundleaction     = 
bundlename       = 
calctol          = 1
changedhash      = 0
check_version    = 0
clean_between_builds = no
command_add_redirect = 1
commanderrfile   = speccmds.err
commandexe       = nest_r_base.none
commandfile      = speccmds.cmd
commandoutfile   = speccmds.out
commandstdoutfile = speccmds.stdout
comparedir       = compare
compareerrfile   = compare.err
comparefile      = compare.cmd
compareoutfile   = compare.out
comparestdoutfile = compare.stdout
compile_error    = 0
compwhite        = 
configdir        = config
configfile       = llvm_noptr_jn_thinlto_split_0416.cfg
configpath       = /data2/jn/spec/spec2026/config/llvm_noptr_jn_thinlto_split_0416.cfg
copies           = 1
coterminal_angles = 
current_range    = 
datadir          = data
default_size     = ref
default_submit   = $command
delay            = 0
deletebinaries   = 0
deletework       = 0
dependent_workloads = 0
device           = 
difflines        = 10
dirprot          = 511
discard_power_samples = 0
enable_monitor   = 1
endian           = 12345678
env_vars         = 0
expand_notes     = 0
expid            = 
exthash_bits     = 256
failflags        = 0
fake             = 0
feedback         = 1
flag_url_base    = https://www.spec.org/auto/cpu2026/Docs/benchmarks/flags/
floatcompare     = 
force_monitor    = 0
fw_bios          = 
hostname         = localhost.localdomain
http_proxy       = 
http_timeout     = 30
hw_avail         = 
hw_cooling       = Air
hw_cpu_max_mhz   = 
hw_cpu_name      = Kunpeng 920 7280Z
hw_cpu_nominal_mhz = 
hw_memory001     = 502.094 GB fixme: If using DDR4, the format is:
hw_memory002     = 'N GB (N x N GB nRxn PC4-nnnnX-X)'
hw_model         = 
hw_nchips        = 2
hw_ncores        = 160
hw_ncpuorder     = 
hw_nthreadspercore = 2
hw_ocache        = 
hw_other         = 
hw_pcache        = 
hw_scache        = 
hw_storage       = 2.9 TB  add more disk info here
hw_tcache        = 
hw_vendor        = Huawei Technologies Co., Ltd.
idle_current_range = 
idledelay        = 10
idleduration     = 60
ignore_errors    = 1
ignore_sigint    = 0
ignorecase       = 
info_wrap_columns = 50
inputdir         = input
inputgenerrfile  = inputgen.err
inputgenfile     = inputgen.cmd
inputgenoutfile  = inputgen.out
inputgenstdoutfile = inputgen.stdout
iterations       = 1
jemalloc_path    = NOT_EXIST
keeptmp          = 0
label            = none
license_num      = 999 (Your SPEC license number)
line_width       = 1020
link_input_files = 1
locking          = 1
log              = CPU2026
log_line_width   = 1020
log_timestamp    = 0
logname          = /data2/jn/spec/spec_test_split/fprate/result/CPU2026.014.log
lognum           = 014
mail_reports     = all
mailcompress     = 0
mailmethod       = smtp
mailport         = 25
mailserver       = 127.0.0.1
mailto           = 
make             = specmake
make_no_clobber  = 0
makefile_template = Makefile.YYYtArGeTYYYspec
makeflags        = --jobs=24
max_average_uncertainty = 1
max_hum_limit    = 0
max_report_runs  = 3
max_unknown_uncertainty = 1
mean_anyway      = 1
meter_connect_timeout = 30
meter_errors_default = 5
meter_errors_percentage = 5
min_report_runs  = 2
min_temp_limit   = 20
minimize_builddirs = 0
minimize_rundirs = 0
multiprocess     = 
name             = nest_r
nansupport       = 
need_math        = 
no_input_handler = close
no_monitor       = 
noratios         = 0
note_preenv      = 0
notes_plat_sysinfo_100 = 
notes_plat_sysinfo_101 =  Sysinfo program /data2/jn/spec/spec2026/bin/sysinfo
notes_plat_sysinfo_102 =  Rev: 069f95da7e7f5d81b2ce48a82150e54f
notes_plat_sysinfo_103 =  running on localhost Fri Apr 17 22:23:34 2026
notes_plat_sysinfo_104 = 
notes_plat_sysinfo_105 =  SUT (System Under Test) info as seen by some common utilities.
notes_plat_sysinfo_107 = 
notes_plat_sysinfo_108 =  ------------------------------------------------------------
notes_plat_sysinfo_109 =  Table of contents
notes_plat_sysinfo_110 =  ------------------------------------------------------------
notes_plat_sysinfo_111 =   1. uname -srvm
notes_plat_sysinfo_112 =   2. w
notes_plat_sysinfo_113 =   3. Username
notes_plat_sysinfo_114 =   4. ulimit -a
notes_plat_sysinfo_115 =   5. sysinfo process ancestry
notes_plat_sysinfo_116 =   6. /proc/cpuinfo
notes_plat_sysinfo_117 =   7. lscpu
notes_plat_sysinfo_118 =   8. numactl --hardware
notes_plat_sysinfo_119 =   9. /proc/meminfo
notes_plat_sysinfo_120 =  10. who -r
notes_plat_sysinfo_121 =  11. Systemd service manager version: systemd 249 (v249-81.oe2203sp4)
notes_plat_sysinfo_122 =  12. Failed units, from systemctl list-units --state=failed
notes_plat_sysinfo_123 =  13. Services, from systemctl list-unit-files
notes_plat_sysinfo_124 =  14. Linux kernel boot-time arguments, from /proc/cmdline
notes_plat_sysinfo_125 =  15. cpupower frequency-info
notes_plat_sysinfo_126 =  16. tuned-adm active
notes_plat_sysinfo_127 =  17. sysctl
notes_plat_sysinfo_128 =  18. /sys/kernel/mm/transparent_hugepage
notes_plat_sysinfo_129 =  19. /sys/kernel/mm/transparent_hugepage/khugepaged
notes_plat_sysinfo_130 =  20. OS release
notes_plat_sysinfo_131 =  21. Disk information
notes_plat_sysinfo_132 =  22. /sys/devices/virtual/dmi/id
notes_plat_sysinfo_133 =  23. dmidecode
notes_plat_sysinfo_134 =  24. BIOS
notes_plat_sysinfo_135 =  ------------------------------------------------------------
notes_plat_sysinfo_206 = 
notes_plat_sysinfo_207 =  ------------------------------------------------------------
notes_plat_sysinfo_208 =  1. uname -srvm
notes_plat_sysinfo_209 =    Linux 5.10.0-299.0.0.202.oe2203sp4.aarch64 \#1 SMP Wed Jan 28 21:00:47 CST 2026 aarch64
notes_plat_sysinfo_210 = 
notes_plat_sysinfo_211 =  ------------------------------------------------------------
notes_plat_sysinfo_212 =  2. w
notes_plat_sysinfo_213 =     22:23:34 up 24 days,  6:36, 23 users,  load average: 2.14, 0.85, 0.65
notes_plat_sysinfo_214 =    USER     TTY        LOGIN@   IDLE   JCPU   PCPU WHAT
notes_plat_sysinfo_215 =    root     pts/1     09Apr26  6:01m  0.14s  0.14s -bash
notes_plat_sysinfo_216 =    root     pts/2     Mon01    4days  0.21s  0.21s -bash
notes_plat_sysinfo_217 =    root     pts/4     Mon23    4:05   0.46s  0.46s -bash
notes_plat_sysinfo_218 =    root     pts/7     30Mar26 17days  0.81s  0.81s -bash
notes_plat_sysinfo_219 =    root     pts/8     Tue10    3days  0.04s  0.04s -bash
notes_plat_sysinfo_220 =    root     pts/9     30Mar26 17days  0.34s  0.34s -bash
notes_plat_sysinfo_221 =    root     pts/10    30Mar26 17days  0.08s  0.08s -bash
notes_plat_sysinfo_222 =    root     pts/11    Wed16   31:48m  0.39s  0.39s -bash
notes_plat_sysinfo_223 =    root     pts/12    Wed16    5:38m  0.07s  0.07s -bash
notes_plat_sysinfo_224 =    root     pts/13    06Apr26 24:48m 34.87s  9.04s perf report
notes_plat_sysinfo_225 =    root     pts/14    Thu10    1:03   0.36s  0.36s -bash
notes_plat_sysinfo_226 =    root     pts/15    31Mar26 14:06m  0.99s  0.71s -bash
notes_plat_sysinfo_227 =    root     pts/16    Thu11   12:40m  0.09s  0.09s -bash
notes_plat_sysinfo_228 =    root     pts/17    Thu14   11:05m  5.04s  4.76s gdb --args ./nest_r_base.none cuba_stdp.sli
notes_plat_sysinfo_229 =    root     pts/20    Thu15    3.00s  2.52s   ?    bash run_split.sh
notes_plat_sysinfo_230 =    root     pts/21    Thu15   11:15m  0.12s  0.12s -bash
notes_plat_sysinfo_231 =    root     pts/22    Thu23   19:43m  0.06s  0.06s -bash
notes_plat_sysinfo_232 =    root     pts/5     03Apr26 19:44m  1.89s  1.65s vim temp.out
notes_plat_sysinfo_233 =    root     pts/23    07Apr26 10days  3:27m  3:27m top
notes_plat_sysinfo_234 =    root     pts/24    14:02    8:10m  0.12s  0.08s vim dev17x_build_compile_time_local_debug.sh
notes_plat_sysinfo_235 =    root     pts/25    16:16   45:23   0.09s  0.09s -bash
notes_plat_sysinfo_236 =    root     pts/26    18:37    7.00s  0.20s  0.20s -bash
notes_plat_sysinfo_237 =    root     pts/6     Mon23    1:27   3.05s  3.05s -bash
notes_plat_sysinfo_238 = 
notes_plat_sysinfo_239 =  ------------------------------------------------------------
notes_plat_sysinfo_240 =  3. Username
notes_plat_sysinfo_241 =    From environment variable $USER:  root
notes_plat_sysinfo_242 = 
notes_plat_sysinfo_243 =  ------------------------------------------------------------
notes_plat_sysinfo_244 =  4. ulimit -a
notes_plat_sysinfo_245 =    real-time non-blocking time  (microseconds, -R) unlimited
notes_plat_sysinfo_246 =    core file size              (blocks, -c) unlimited
notes_plat_sysinfo_247 =    data seg size               (kbytes, -d) unlimited
notes_plat_sysinfo_248 =    scheduling priority                 (-e) 0
notes_plat_sysinfo_249 =    file size                   (blocks, -f) unlimited
notes_plat_sysinfo_250 =    pending signals                     (-i) 2056464
notes_plat_sysinfo_251 =    max locked memory           (kbytes, -l) unlimited
notes_plat_sysinfo_252 =    max memory size             (kbytes, -m) unlimited
notes_plat_sysinfo_253 =    open files                          (-n) 1000000000
notes_plat_sysinfo_254 =    pipe size                (512 bytes, -p) 8
notes_plat_sysinfo_255 =    POSIX message queues         (bytes, -q) 819200
notes_plat_sysinfo_256 =    real-time priority                  (-r) 0
notes_plat_sysinfo_257 =    stack size                  (kbytes, -s) unlimited
notes_plat_sysinfo_258 =    cpu time                   (seconds, -t) unlimited
notes_plat_sysinfo_259 =    max user processes                  (-u) 2056464
notes_plat_sysinfo_260 =    virtual memory              (kbytes, -v) unlimited
notes_plat_sysinfo_261 =    file locks                          (-x) unlimited
notes_plat_sysinfo_262 = 
notes_plat_sysinfo_263 =  ------------------------------------------------------------
notes_plat_sysinfo_264 =  5. sysinfo process ancestry
notes_plat_sysinfo_265 =   /usr/lib/systemd/systemd --switched-root --system --deserialize 16
notes_plat_sysinfo_266 =   sshd: /usr/sbin/sshd -D [listener] 0 of 10-100 startups
notes_plat_sysinfo_267 =   sshd: root [priv]
notes_plat_sysinfo_268 =   sshd: root@pts/20
notes_plat_sysinfo_269 =   -bash
notes_plat_sysinfo_270 =   bash run_split.sh
notes_plat_sysinfo_271 =   runcpu --config=llvm_noptr_jn_thinlto_split_0416.cfg --rebuild --iterations=1 --copies=1 --action=run
notes_plat_sysinfo_272 =     --tune=base -S LTO=thin -S mathlib=0 -S debug=1 --output_root=/data2/jn/spec/spec_test_split/fprate 767
notes_plat_sysinfo_273 =   specperl $SPEC/bin/sysinfo
notes_plat_sysinfo_274 =  $SPEC = /data2/jn/spec/spec2026
notes_plat_sysinfo_275 = 
notes_plat_sysinfo_276 =  ------------------------------------------------------------
notes_plat_sysinfo_277 =  6. /proc/cpuinfo
notes_plat_sysinfo_278 =      CPU implementer : 0x48
notes_plat_sysinfo_279 =      CPU architecture: 8
notes_plat_sysinfo_280 =      CPU variant     : 0x0
notes_plat_sysinfo_281 =      CPU part        : 0xd02
notes_plat_sysinfo_282 =      CPU revision    : 0
notes_plat_sysinfo_283 =      Features        : fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm jscvt
notes_plat_sysinfo_284 =                        fcma lrcpc dcpop sha3 sm3 sm4 asimddp sha512 sve asimdfhm dit uscat ilrcpc flagm ssbs
notes_plat_sysinfo_285 =                        sb dcpodp flagm2 frint svei8mm svef32mm svef64mm svebf16 i8mm bf16 dgh rng ecv
notes_plat_sysinfo_286 = 
notes_plat_sysinfo_287 =  ------------------------------------------------------------
notes_plat_sysinfo_288 =  7. lscpu
notes_plat_sysinfo_289 = 
notes_plat_sysinfo_290 =  From lscpu from util-linux 2.37.2:
notes_plat_sysinfo_291 =    Architecture:                       aarch64
notes_plat_sysinfo_292 =    CPU op-mode(s):                     64-bit
notes_plat_sysinfo_293 =    Byte Order:                         Little Endian
notes_plat_sysinfo_294 =    CPU(s):                             320
notes_plat_sysinfo_295 =    On-line CPU(s) list:                0-319
notes_plat_sysinfo_296 =    Vendor ID:                          HiSilicon
notes_plat_sysinfo_297 =    BIOS Vendor ID:                     HiSilicon
notes_plat_sysinfo_298 =    Model name:                         Kunpeng 920 7280Z
notes_plat_sysinfo_299 =    BIOS Model name:                    Kunpeng 920 7280Z
notes_plat_sysinfo_300 =    Model:                              0
notes_plat_sysinfo_301 =    Thread(s) per core:                 2
notes_plat_sysinfo_302 =    Core(s) per socket:                 80
notes_plat_sysinfo_303 =    Socket(s):                          2
notes_plat_sysinfo_304 =    Stepping:                           0x0
notes_plat_sysinfo_305 =    Frequency boost:                    disabled
notes_plat_sysinfo_306 =    CPU max MHz:                        2900.0000
notes_plat_sysinfo_307 =    CPU min MHz:                        400.0000
notes_plat_sysinfo_308 =    BogoMIPS:                           200.00
notes_plat_sysinfo_309 =    Flags:                              fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid
notes_plat_sysinfo_310 =                                        asimdrdm jscvt fcma lrcpc dcpop sha3 sm3 sm4 asimddp sha512 sve
notes_plat_sysinfo_311 =                                        asimdfhm dit uscat ilrcpc flagm ssbs sb dcpodp flagm2 frint svei8mm
notes_plat_sysinfo_312 =                                        svef32mm svef64mm svebf16 i8mm bf16 dgh rng ecv
notes_plat_sysinfo_313 =    L1d cache:                          10 MiB (160 instances)
notes_plat_sysinfo_314 =    L1i cache:                          10 MiB (160 instances)
notes_plat_sysinfo_315 =    L2 cache:                           200 MiB (160 instances)
notes_plat_sysinfo_316 =    L3 cache:                           280 MiB (4 instances)
notes_plat_sysinfo_317 =    NUMA node(s):                       4
notes_plat_sysinfo_318 =    NUMA node0 CPU(s):                  0-79
notes_plat_sysinfo_319 =    NUMA node1 CPU(s):                  80-159
notes_plat_sysinfo_320 =    NUMA node2 CPU(s):                  160-239
notes_plat_sysinfo_321 =    NUMA node3 CPU(s):                  240-319
notes_plat_sysinfo_322 =    Vulnerability Gather data sampling: Not affected
notes_plat_sysinfo_323 =    Vulnerability Itlb multihit:        Not affected
notes_plat_sysinfo_324 =    Vulnerability L1tf:                 Not affected
notes_plat_sysinfo_325 =    Vulnerability Mds:                  Not affected
notes_plat_sysinfo_326 =    Vulnerability Meltdown:             Not affected
notes_plat_sysinfo_327 =    Vulnerability Mmio stale data:      Not affected
notes_plat_sysinfo_328 =    Vulnerability Retbleed:             Not affected
notes_plat_sysinfo_329 =    Vulnerability Spec rstack overflow: Not affected
notes_plat_sysinfo_330 =    Vulnerability Spec store bypass:    Not affected
notes_plat_sysinfo_331 =    Vulnerability Spectre v1:           Mitigation; __user pointer sanitization
notes_plat_sysinfo_332 =    Vulnerability Spectre v2:           Not affected
notes_plat_sysinfo_333 =    Vulnerability Srbds:                Not affected
notes_plat_sysinfo_334 =    Vulnerability Tsx async abort:      Not affected
notes_plat_sysinfo_335 = 
notes_plat_sysinfo_336 =  From lscpu --cache:
notes_plat_sysinfo_337 =       NAME ONE-SIZE ALL-SIZE WAYS TYPE        LEVEL SETS PHY-LINE COHERENCY-SIZE
notes_plat_sysinfo_338 =       L1d       64K      10M    4 Data            1  256                      64
notes_plat_sysinfo_339 =       L1i       64K      10M    4 Instruction     1  256                      64
notes_plat_sysinfo_340 =       L2       1.3M     200M   10 Unified         2 2048                      64
notes_plat_sysinfo_341 =       L3        70M     280M   28 Unified         3 2048                     128
notes_plat_sysinfo_342 = 
notes_plat_sysinfo_343 =  ------------------------------------------------------------
notes_plat_sysinfo_344 =  8. numactl --hardware
notes_plat_sysinfo_345 =  NOTE: a numactl 'node' might or might not correspond to a physical chip.
notes_plat_sysinfo_346 =    available: 4 nodes (0-3)
notes_plat_sysinfo_347 =    node 0 cpus: 0-79
notes_plat_sysinfo_348 =    node 0 size: 128884 MB
notes_plat_sysinfo_349 =    node 0 free: 81679 MB
notes_plat_sysinfo_350 =    node 1 cpus: 80-159
notes_plat_sysinfo_351 =    node 1 size: 128327 MB
notes_plat_sysinfo_352 =    node 1 free: 61086 MB
notes_plat_sysinfo_353 =    node 2 cpus: 160-239
notes_plat_sysinfo_354 =    node 2 size: 127968 MB
notes_plat_sysinfo_355 =    node 2 free: 87932 MB
notes_plat_sysinfo_356 =    node 3 cpus: 240-319
notes_plat_sysinfo_357 =    node 3 size: 128964 MB
notes_plat_sysinfo_358 =    node 3 free: 86325 MB
notes_plat_sysinfo_359 =    node distances:
notes_plat_sysinfo_360 =    node   0   1   2   3
notes_plat_sysinfo_361 =      0:  10  12  35  37
notes_plat_sysinfo_362 =      1:  12  10  37  40
notes_plat_sysinfo_363 =      2:  35  37  10  12
notes_plat_sysinfo_364 =      3:  37  40  12  10
notes_plat_sysinfo_365 = 
notes_plat_sysinfo_366 =  ------------------------------------------------------------
notes_plat_sysinfo_367 =  9. /proc/meminfo
notes_plat_sysinfo_368 =     MemTotal:       526483764 kB
notes_plat_sysinfo_369 = 
notes_plat_sysinfo_370 =  ------------------------------------------------------------
notes_plat_sysinfo_371 =  10. who -r
notes_plat_sysinfo_372 =    run-level 3 Mar 24 15:48
notes_plat_sysinfo_373 = 
notes_plat_sysinfo_374 =  ------------------------------------------------------------
notes_plat_sysinfo_375 =  11. Systemd service manager version: systemd 249 (v249-81.oe2203sp4)
notes_plat_sysinfo_376 =    Default Target  Status
notes_plat_sysinfo_377 =    multi-user      degraded
notes_plat_sysinfo_378 = 
notes_plat_sysinfo_379 =  ------------------------------------------------------------
notes_plat_sysinfo_380 =  12. Failed units, from systemctl list-units --state=failed
notes_plat_sysinfo_381 =      UNIT                               LOAD   ACTIVE SUB    DESCRIPTION
notes_plat_sysinfo_382 =    * auditd.service                     loaded failed failed Security Auditing Service
notes_plat_sysinfo_383 =    * NetworkManager-wait-online.service loaded failed failed Network Manager Wait Online
notes_plat_sysinfo_384 = 
notes_plat_sysinfo_385 =  ------------------------------------------------------------
notes_plat_sysinfo_386 =  13. Services, from systemctl list-unit-files
notes_plat_sysinfo_387 =    STATE            UNIT FILES
notes_plat_sysinfo_388 =    enabled          NetworkManager NetworkManager-dispatcher NetworkManager-wait-online atd auditd chronyd
notes_plat_sysinfo_389 =                     crond docker getty@ kdump lm_sensors lvm2-monitor mdmonitor oeaware openibd restorecond
notes_plat_sysinfo_390 =                     rngd rshim rsyslog rtkit-daemon smartd sshd sysstat systemtap tuned
notes_plat_sysinfo_391 =    enabled-runtime  systemd-fsck-root systemd-remount-fs
notes_plat_sysinfo_392 =    disabled         arp-ethers blk-availability chrony-wait console-getty cpupower debug-shell dhcpd dhcpd6
notes_plat_sysinfo_393 =                     dhcrelay e2scrub_reap fancontrol firewalld httpd httpd@ hwclock-save ibacm import-state
notes_plat_sysinfo_394 =                     ip6tables iprdump iprinit iprupdate ipset iptables irqbalance lldpad loadmodules
notes_plat_sysinfo_395 =                     low-memory-monitor lvm2-lvmdbusd multipathd ndctl-monitor nftables nis-domainname
notes_plat_sysinfo_396 =                     nm-cloud-setup nvmefc-boot-connections nvmf-autoconnect openEuler-security opensmd
notes_plat_sysinfo_397 =                     opensmd@ ostree-remount pesign phoromatic-client phoromatic-server phoronix-result-server
notes_plat_sysinfo_398 =                     powertop rsyncd saslauthd selinux-autorelabel-mark srp_daemon srp_daemon_port@
notes_plat_sysinfo_399 =                     sshd-keygen@ sssd systemd-boot-check-no-failures systemd-network-generator
notes_plat_sysinfo_400 =                     systemd-resolved tcsd
notes_plat_sysinfo_401 =    generated        mst
notes_plat_sysinfo_402 =    indirect         serial-getty@ sssd-autofs sssd-kcm sssd-nss sssd-pac sssd-pam sssd-ssh sssd-sudo
notes_plat_sysinfo_403 =    masked           timedatex
notes_plat_sysinfo_404 =    transient        lvm-activate-openeuler
notes_plat_sysinfo_405 = 
notes_plat_sysinfo_406 =  ------------------------------------------------------------
notes_plat_sysinfo_407 =  14. Linux kernel boot-time arguments, from /proc/cmdline
notes_plat_sysinfo_408 =    BOOT_IMAGE=/vmlinuz-5.10.0-299.0.0.202.oe2203sp4.aarch64
notes_plat_sysinfo_409 =    root=/dev/mapper/openeuler-root
notes_plat_sysinfo_410 =    ro
notes_plat_sysinfo_411 =    rd.lvm.lv=openeuler/root
notes_plat_sysinfo_412 =    rd.lvm.lv=openeuler/swap
notes_plat_sysinfo_413 =    video=VGA-1:640x480-32@60me
notes_plat_sysinfo_414 =    cgroup_disable=files
notes_plat_sysinfo_415 =    apparmor=0
notes_plat_sysinfo_416 =    crashkernel=1024M,high
notes_plat_sysinfo_417 =    smmu.bypassdev=0x1000:0x17
notes_plat_sysinfo_418 =    smmu.bypassdev=0x1000:0x15
notes_plat_sysinfo_419 =    arm64.nopauth
notes_plat_sysinfo_420 =    console=tty0
notes_plat_sysinfo_421 =    default_hugepagesz=512M
notes_plat_sysinfo_422 =    hugepagesz=512M
notes_plat_sysinfo_423 =    hugepages=100
notes_plat_sysinfo_424 =    iommu.passthrough=1
notes_plat_sysinfo_425 = 
notes_plat_sysinfo_426 =  ------------------------------------------------------------
notes_plat_sysinfo_427 =  15. cpupower frequency-info
notes_plat_sysinfo_428 =    analyzing CPU 0:
notes_plat_sysinfo_429 =      current policy: frequency should be within 400 MHz and 2.90 GHz.
notes_plat_sysinfo_430 =                      The governor "performance" may decide which speed to use
notes_plat_sysinfo_431 =                      within this range.
notes_plat_sysinfo_432 = 
notes_plat_sysinfo_433 =  ------------------------------------------------------------
notes_plat_sysinfo_434 =  16. tuned-adm active
notes_plat_sysinfo_435 =    Current active profile: throughput-performance
notes_plat_sysinfo_436 = 
notes_plat_sysinfo_437 =  ------------------------------------------------------------
notes_plat_sysinfo_438 =  17. sysctl
notes_plat_sysinfo_439 =    kernel.numa_balancing               1
notes_plat_sysinfo_440 =    kernel.randomize_va_space           2
notes_plat_sysinfo_441 =    vm.compaction_proactiveness        20
notes_plat_sysinfo_442 =    vm.dirty_background_bytes           0
notes_plat_sysinfo_443 =    vm.dirty_background_ratio          10
notes_plat_sysinfo_444 =    vm.dirty_bytes                      0
notes_plat_sysinfo_445 =    vm.dirty_expire_centisecs        3000
notes_plat_sysinfo_446 =    vm.dirty_ratio                     60
notes_plat_sysinfo_447 =    vm.dirty_writeback_centisecs      500
notes_plat_sysinfo_448 =    vm.dirtytime_expire_seconds     43200
notes_plat_sysinfo_449 =    vm.extfrag_threshold              500
notes_plat_sysinfo_450 =    vm.min_unmapped_ratio               1
notes_plat_sysinfo_451 =    vm.nr_hugepages                   100
notes_plat_sysinfo_452 =    vm.nr_hugepages_mempolicy         100
notes_plat_sysinfo_453 =    vm.nr_overcommit_hugepages          0
notes_plat_sysinfo_454 =    vm.swappiness                      10
notes_plat_sysinfo_455 =    vm.watermark_boost_factor       15000
notes_plat_sysinfo_456 =    vm.watermark_scale_factor          10
notes_plat_sysinfo_457 =    vm.zone_reclaim_mode                0
notes_plat_sysinfo_458 = 
notes_plat_sysinfo_459 =  ------------------------------------------------------------
notes_plat_sysinfo_460 =  18. /sys/kernel/mm/transparent_hugepage
notes_plat_sysinfo_461 =    defrag          always defer defer+madvise [madvise] never
notes_plat_sysinfo_462 =    enabled         [always] madvise never
notes_plat_sysinfo_463 =    hpage_pmd_size  2097152
notes_plat_sysinfo_464 =    shmem_enabled   always within_size advise [never] deny force
notes_plat_sysinfo_465 = 
notes_plat_sysinfo_466 =  ------------------------------------------------------------
notes_plat_sysinfo_467 =  19. /sys/kernel/mm/transparent_hugepage/khugepaged
notes_plat_sysinfo_468 =    alloc_sleep_millisecs   60000
notes_plat_sysinfo_469 =    defrag                      1
notes_plat_sysinfo_470 =    max_ptes_none             511
notes_plat_sysinfo_471 =    max_ptes_shared           256
notes_plat_sysinfo_472 =    max_ptes_swap              64
notes_plat_sysinfo_473 =    pages_to_scan            4096
notes_plat_sysinfo_474 =    scan_sleep_millisecs    10000
notes_plat_sysinfo_475 = 
notes_plat_sysinfo_476 =  ------------------------------------------------------------
notes_plat_sysinfo_477 =  20. OS release
notes_plat_sysinfo_478 =    From /etc/*-release /etc/*-version
notes_plat_sysinfo_479 =    os-release        openEuler 22.03 (LTS-SP4)
notes_plat_sysinfo_480 =    openEuler-release openEuler release 22.03 (LTS-SP4)
notes_plat_sysinfo_481 =    system-release    openEuler release 22.03 (LTS-SP4)
notes_plat_sysinfo_482 = 
notes_plat_sysinfo_483 =  ------------------------------------------------------------
notes_plat_sysinfo_484 =  21. Disk information
notes_plat_sysinfo_485 =  SPEC is set to: /data2/jn/spec/spec2026
notes_plat_sysinfo_486 =    Filesystem     Type  Size  Used Avail Use% Mounted on
notes_plat_sysinfo_487 =    /dev/nvme3n1   ext4  2.9T  2.5T  313G  89% /data2
notes_plat_sysinfo_488 = 
notes_plat_sysinfo_489 =  ------------------------------------------------------------
notes_plat_sysinfo_490 =  22. /sys/devices/virtual/dmi/id
notes_plat_sysinfo_491 =      Product:        S920X20
notes_plat_sysinfo_492 =      Serial:         2102315ALG10QB100111
notes_plat_sysinfo_493 = 
notes_plat_sysinfo_494 =  ------------------------------------------------------------
notes_plat_sysinfo_495 =  23. dmidecode
notes_plat_sysinfo_496 =    Additional information from dmidecode 3.4 follows.  WARNING: Use caution when you interpret this section.
notes_plat_sysinfo_497 =    The 'dmidecode' program reads system data which is "intended to allow hardware to be accurately
notes_plat_sysinfo_498 =    determined", but the intent may not be met, as there are frequent changes to hardware, firmware, and the
notes_plat_sysinfo_499 =    "DMTF SMBIOS" standard.
notes_plat_sysinfo_500 =    Memory:
notes_plat_sysinfo_501 =      16x Hynix HMCG88AEBRA115N 32 GB 2 rank 4800
notes_plat_sysinfo_502 = 
notes_plat_sysinfo_503 = 
notes_plat_sysinfo_504 =  ------------------------------------------------------------
notes_plat_sysinfo_505 =  24. BIOS
notes_plat_sysinfo_506 =  (This section combines info from /sys/devices and dmidecode.)
notes_plat_sysinfo_507 =     BIOS Vendor:       Huawei Corp.
notes_plat_sysinfo_508 =     BIOS Version:      21.37
notes_plat_sysinfo_509 =     BIOS Date:         10/18/2025
notes_plat_sysinfo_510 =     BIOS Revision:     21.37
notes_wrap_columns = 0
notes_wrap_indent =   
num              = 767
obiwan           = 
os_exe_ext       = 
output_format    = txt,html,cfg,pdf,csv
output_root      = /data2/jn/spec/spec_test_split/fprate
outputdir        = output
parallel_test    = 0
parallel_test_submit = 0
parallel_test_workloads = 
path             = /data2/jn/spec/spec2026/benchspec/CPU/767.nest_r
plain_train      = 1
platform         = 
power            = 0
power_management = 
preENV_HUGETLB_MORECORE = yes
preENV_LD_LIBRARY_PATH = %{llvm_dir}/lib64/:%{llvm_dir}/lib/:/lib64
preENV_LD_PRELOAD = /usr/lib64/libhugetlbfs.so
preENV_PATH      = /data2/jn/project/oe_llvm/llvm-project/thinlto-split/bin:%{ENV_PATH}
preenv           = LD_PRELOAD=
prefix           = 
prepared_by      = root  (is never output, only tags rawfile)
ranks            = 1
rawhash_bits     = 256
rebuild          = 1
reftime          = reftime
reltol           = 
reportable       = 0
resultdir        = result
review           = 0
run              = all
runcpu           = /data2/jn/spec/spec2026/bin/harness/runcpu --config=llvm_noptr_jn_thinlto_split_0416.cfg --rebuild --iterations=1 --copies=1 --action=run --tune=base -S LTO=thin -S mathlib=0 -S debug=1 --output_root=/data2/jn/spec/spec_test_split/fprate 767
rundir           = run
runmode          = rate
safe_eval        = 1
save_build_files = 
section_specifier_fatal = 1
setprocgroup     = 1
setup_error      = 0
sigint           = 2
size             = refrate
size_class       = ref
skipabstol       = 
skipobiwan       = 
skipreltol       = 
skiptol          = 
smarttune        = base
specdiff         = specdiff
specrun          = specinvoke
srcalt           = 
srcdir           = src
srcsource        = /data2/jn/spec/spec2026/benchspec/CPU/767.nest_r/src
stagger          = 10
strict_rundir_verify = 0
submit_default   = numactl --localalloc --physcpubind=256 $command
sw_avail         = 
sw_base_ptrsize  = 64-bit
sw_compiler001   = C/C++/Fortran: Version 20.1.8 of LLVM
sw_compiler_community_support = Yes
sw_file          = ext4
sw_os001         = openEuler 22.03 (LTS-SP4)
sw_os002         = 5.10.0-299.0.0.202.oe2203sp4.aarch64
sw_other         = 
sw_peak_ptrsize  = Not Applicable
sw_state         = Run level 3 (add definition here)
sysinfo_cmd      = /bin/true
sysinfo_hash_bits = 256
sysinfo_program  = specperl /data2/jn/spec/spec2026/bin/sysinfo
sysinfo_program_hash = sysinfo:SHA:48a8d7b01266a01cbc82d54e6ed932a3aef19f427c374d8a35ccb8e3a4d4098a
table            = 1
teeout           = 0
test_date        = Apr-2026
test_sponsor     = Huawei Technologies Co., Ltd.
tester           = Huawei Technologies Co., Ltd.
threads          = 1
top              = /data2/jn/spec/spec2026
train_single_thread = 0
train_with       = train
tune             = base
uid              = 0
unbuffer         = 1
uncertainty_exception = 5
update           = 0
update_url       = http://www.spec.org/auto/cpu2026/updates/
use_submit_for_compare = 0
use_submit_for_speed = 0
username         = root
verbose          = 5
verify_binaries  = 1
version          = 0.902000
voltage_range    = 
worklist         = list
OUTPUT_RMFILES   = balancedneuron-2.sli.out cuba-4002-0.dat cuba-4003-0.dat cuba.sli.out cuba_ps-4002-0.dat cuba_ps-4003-0.dat cuba_ps.sli.out
