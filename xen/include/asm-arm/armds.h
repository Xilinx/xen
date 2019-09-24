#define _start                      Load$$_text$$Base
#define _stext                      Load$$_text$$Base

#define _etext                      Load$$_text$$Limit

//#define _srodata                    Load$$_rodata_bug_frames_0$$Base
#define __start_bug_frames          Load$$_rodata_bug_frames_0$$Base

#define __stop_bug_frames_0         Load$$_rodata_bug_frames_0$$Limit
#define __stop_bug_frames_1         Load$$_rodata_bug_frames_1$$Limit
#define __stop_bug_frames_2         Load$$_rodata_bug_frames_2$$Limit

#ifdef CONFIG_LOCK_PROFILE
#define __lock_profile_start        Load$$_rodata_lockprofile_data$$Base
#define __lock_profile_end          Load$$_rodata_lockprofile_data$$Limit
#endif

#define __param_start               Load$$_rodata_data_param$$Base
#define __param_end                 Load$$_rodata_data_param$$Limit

#define __proc_info_start           Load$$_rodata_proc_info$$Base
#define __proc_info_end             Load$$_rodata_proc_info$$Limit

#define _erodata                    Load$$_rodata_proc_info$$Limit

#if defined(CONFIG_HAS_VPCI) && defined(CONFIG_LATE_HWDOM)
#define __start_vpci_array          Load$$_rodata_data_vpci$$Base
#define __end_vpci_array            Load$$_rodata_data_vpci$$Limit

#undef _erodata
#define _erodata                    Load$$_rodata_data_vpci$$Limit
#endif

#if defined(BUILD_ID)
#define __note_gnu_build_id_start   Load$$_note_gnu_build_id$$Base
#define __note_gnu_build_id_end     Load$$_note_gnu_build_id$$Limit

#undef _erodata
#define _erodata                    Load$$_note_gnu_build_id$$Limit
#endif

#define __start_schedulers_array    Load$$_data_schedulers$$Base
#define __end_schedulers_array      Load$$_data_schedulers$$Limit

/* Does not exist for ARM
#define __start___ex_table          Load$$_data_ex_table$$Base
#define __stop___ex_table           Load$$_data_ex_table$$Limit
*/

#define __start___pre_ex_table      Load$$_data_ex_table_pre$$Base
#define __stop___pre_ex_table       Load$$_data_ex_table_pre$$Limit

#define _splatform                  Load$$_arch_info$$Base
#define _eplatform                  Load$$_arch_info$$Limit

#define _sdevice                    Load$$_dev_info$$Base
#define _edevice                    Load$$_dev_info$$Limit

#define _asdevice                   Load$$_adev_info$$Base
#define _aedevice                   Load$$_adev_info$$Limit

#define __init_begin                Load$$_init_text$$Base
#define _sinittext                  Load$$_init_text$$Base
#define _einittext                  Load$$_init_text$$Limit

#define __setup_start               Load$$_init_setup$$Base
#define __setup_end                 Load$$_init_setup$$Limit

#define __initcall_start            Load$$_initcallpresmp_init$$Base
#define __presmp_initcall_end       Load$$_initcallpresmp_init$$Limit
#define __initcall_end              Load$$_initcall1_init$$Limit

#define __alt_instructions          Load$$_altinstructions$$Base
#define __alt_instructions_end      Load$$_altinstructions$$Limit

#define __ctors_start               Load$$_ctors$$Base
#define __ctors_end                 Load$$_init_array_sorted$$Limit
#define __init_end_efi              Load$$_init_array_sorted$$Limit

#if defined(CONFIG_HAS_VPCI) && !defined(CONFIG_LATE_HWDOM)
#undef __init_end_efi
#define __init_end_efi              Load$$_data_vpci$$Limit
#endif

#define __init_end                  Load$$_bss$$Base
#define __bss_start                 Load$$_bss$$Base

#define __per_cpu_start             Load$$_bss_percpu$$Base
#define __per_cpu_data_end          Load$$_bss_percpu$$Limit
#define __bss_end                   Load$$_bss_percpu$$Limit
#define _end                        Load$$_bss_percpu$$Limit






#if 0
#define __2snprintf
#define _printf_c
#define _printf_percent
#define _printf_str
#define _printf_d
#define _printf_flags
#define _printf_int_dec
#define _printf_llu
#define _printf_llx
#define _printf_longlong_dec
#define _printf_longlong_hex
#define _printf_pre_padding
#define _printf_sizespec
#define _printf_u
#define _printf_widthprec
#define _printf_return_value
#define _printf_s
#endif
