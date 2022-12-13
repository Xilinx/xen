/*
 * Copyright 2009-2017 Citrix Ltd and other contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; version 2.1 only. with the special
 * exception on linking described in file LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 */

#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include <libxl.h>
#include <libxl_utils.h>
#include <libxlutil.h>

#include "xl.h"
#include "xl_utils.h"
#include "xl_parse.h"
#include <xenstore.h>
#include <xengnttab.h>

static int fd_lock = -1;

static void pause_domain(uint32_t domid)
{
    libxl_domain_pause(ctx, domid, NULL);
}

static void unpause_domain(uint32_t domid)
{
    libxl_domain_unpause(ctx, domid, NULL);
}

static void destroy_domain(uint32_t domid, int force)
{
    int rc;

    if (domid == 0 && !force) {
        fprintf(stderr, "Not destroying domain 0; use -f to force.\n"
                        "This can only be done when using a disaggregated "
                        "hardware domain and toolstack.\n\n");
        exit(EXIT_FAILURE);
    }
    rc = libxl_domain_destroy(ctx, domid, 0);
    if (rc) { fprintf(stderr,"destroy failed (rc=%d)\n",rc); exit(EXIT_FAILURE); }
}

int main_pause(int argc, char **argv)
{
    int opt;

    SWITCH_FOREACH_OPT(opt, "", NULL, "pause", 1) {
        /* No options */
    }

    pause_domain(find_domain(argv[optind]));

    return EXIT_SUCCESS;
}

int main_unpause(int argc, char **argv)
{
    int opt;

    SWITCH_FOREACH_OPT(opt, "", NULL, "unpause", 1) {
        /* No options */
    }

    unpause_domain(find_domain(argv[optind]));

    return EXIT_SUCCESS;
}

int main_destroy(int argc, char **argv)
{
    int opt;
    int force = 0;

    SWITCH_FOREACH_OPT(opt, "f", NULL, "destroy", 1) {
    case 'f':
        force = 1;
        break;
    }

    destroy_domain(find_domain(argv[optind]), force);
    return EXIT_SUCCESS;
}

static void reboot_domain(uint32_t domid, libxl_evgen_domain_death **deathw,
                          libxl_ev_user for_user, int fallback_trigger)
{
    int rc;

    fprintf(stderr, "Rebooting domain %u\n", domid);
    rc = libxl_domain_reboot(ctx, domid, NULL);
    if (rc == ERROR_NOPARAVIRT) {
        if (fallback_trigger) {
            fprintf(stderr, "PV control interface not available:"
                    " sending ACPI reset button event.\n");
            rc = libxl_send_trigger(ctx, domid, LIBXL_TRIGGER_RESET, 0, NULL);
        } else {
            fprintf(stderr, "PV control interface not available:"
                    " external graceful reboot not possible.\n");
            fprintf(stderr, "Use \"-F\" to fallback to ACPI reset event.\n");
        }
    }
    if (rc) {
        fprintf(stderr,"reboot failed (rc=%d)\n",rc);exit(EXIT_FAILURE);
    }

    if (deathw) {
        rc = libxl_evenable_domain_death(ctx, domid, for_user, deathw);
        if (rc) {
            fprintf(stderr,"wait for death failed (evgen, rc=%d)\n",rc);
            exit(EXIT_FAILURE);
        }
    }
}

static void shutdown_domain(uint32_t domid,
                            libxl_evgen_domain_death **deathw,
                            libxl_ev_user for_user,
                            int fallback_trigger)
{
    int rc;

    fprintf(stderr, "Shutting down domain %u\n", domid);
    rc = libxl_domain_shutdown(ctx, domid, NULL);
    if (rc == ERROR_NOPARAVIRT) {
        if (fallback_trigger) {
            fprintf(stderr, "PV control interface not available:"
                    " sending ACPI power button event.\n");
            rc = libxl_send_trigger(ctx, domid, LIBXL_TRIGGER_POWER, 0, NULL);
        } else {
            fprintf(stderr, "PV control interface not available:"
                    " external graceful shutdown not possible.\n");
            fprintf(stderr, "Use \"-F\" to fallback to ACPI power event.\n");
        }
    }

    if (rc) {
        fprintf(stderr,"shutdown failed (rc=%d)\n",rc);exit(EXIT_FAILURE);
    }

    if (deathw) {
        rc = libxl_evenable_domain_death(ctx, domid, for_user, deathw);
        if (rc) {
            fprintf(stderr,"wait for death failed (evgen, rc=%d)\n",rc);
            exit(EXIT_FAILURE);
        }
    }
}

static void wait_for_domain_deaths(libxl_evgen_domain_death **deathws, int nr,
                                   int wait_for_shutdown_or_death)
{
    int rc, count = 0;
    LOG("Waiting for %d domains", nr);
    while(1 && count < nr) {
        libxl_event *event;
        rc = libxl_event_wait(ctx, &event, LIBXL_EVENTMASK_ALL, 0,0);
        if (rc) {
            LOG("Failed to get event, quitting (rc=%d)", rc);
            exit(EXIT_FAILURE);
        }

        switch (event->type) {
        case LIBXL_EVENT_TYPE_DOMAIN_DEATH:
            LOG("Domain %d has been destroyed", event->domid);
            libxl_evdisable_domain_death(ctx, deathws[event->for_user]);
            count++;
            break;
        case LIBXL_EVENT_TYPE_DOMAIN_SHUTDOWN:
            LOG("Domain %d has been shut down, reason code %d",
                event->domid, event->u.domain_shutdown.shutdown_reason);
            if (wait_for_shutdown_or_death) {
                libxl_evdisable_domain_death(ctx, deathws[event->for_user]);
                count++;
            } else {
                LOG("Domain %d continue waiting for death", event->domid);
            }
            break;
        default:
            LOG("Unexpected event type %d", event->type);
            break;
        }
        libxl_event_free(ctx, event);
    }
}

static int main_shutdown_or_reboot(int do_reboot, int argc, char **argv)
{
    const char *what = do_reboot ? "reboot" : "shutdown";
    void (*fn)(uint32_t domid,
               libxl_evgen_domain_death **, libxl_ev_user, int) =
        do_reboot ? &reboot_domain : &shutdown_domain;
    int opt, i, nb_domain;
    int wait_for_it = 0, all = 0, nrdeathws = 0;
    int fallback_trigger = 0;
    static struct option opts[] = {
        {"all", 0, 0, 'a'},
        {"wait", 0, 0, 'w'},
        COMMON_LONG_OPTS
    };

    SWITCH_FOREACH_OPT(opt, "awF", opts, what, 0) {
    case 'a':
        all = 1;
        break;
    case 'w':
        wait_for_it++;
        break;
    case 'F':
        fallback_trigger = 1;
        break;
    }

    if (!argv[optind] && !all) {
        fprintf(stderr, "You must specify -a or a domain id.\n\n");
        return EXIT_FAILURE;
    }

    if (all) {
        libxl_dominfo *dominfo;
        libxl_evgen_domain_death **deathws = NULL;
        if (!(dominfo = libxl_list_domain(ctx, &nb_domain))) {
            fprintf(stderr, "libxl_list_domain failed.\n");
            return EXIT_FAILURE;
        }

        if (wait_for_it)
            deathws = calloc(nb_domain, sizeof(*deathws));

        for (i = 0; i<nb_domain; i++) {
            if (dominfo[i].domid == 0 || dominfo[i].never_stop)
                continue;
            fn(dominfo[i].domid, deathws ? &deathws[i] : NULL, i,
               fallback_trigger);
            nrdeathws++;
        }

        if (deathws) {
            wait_for_domain_deaths(deathws, nrdeathws, wait_for_it == 1);
            free(deathws);
        }

        libxl_dominfo_list_free(dominfo, nb_domain);
    } else {
        libxl_evgen_domain_death *deathw = NULL;
        uint32_t domid = find_domain(argv[optind]);

        fn(domid, wait_for_it ? &deathw : NULL, 0, fallback_trigger);

        if (wait_for_it)
            wait_for_domain_deaths(&deathw, 1, wait_for_it == 1);
    }


    return EXIT_SUCCESS;
}

int main_shutdown(int argc, char **argv)
{
    return main_shutdown_or_reboot(0, argc, argv);
}

int main_reboot(int argc, char **argv)
{
    return main_shutdown_or_reboot(1, argc, argv);
}

static void evdisable_disk_ejects(libxl_evgen_disk_eject **diskws,
                                 int num_disks)
{
    int i;

    for (i = 0; i < num_disks; i++) {
        if (diskws[i])
            libxl_evdisable_disk_eject(ctx, diskws[i]);
        diskws[i] = NULL;
    }
}

static int domain_wait_event(uint32_t domid, libxl_event **event_r)
{
    int ret;
    for (;;) {
        ret = libxl_event_wait(ctx, event_r, LIBXL_EVENTMASK_ALL, 0,0);
        if (ret) {
            LOG("Domain %u, failed to get event, quitting (rc=%d)", domid, ret);
            return ret;
        }
        if ((*event_r)->domid != domid) {
            char *evstr = libxl_event_to_json(ctx, *event_r);
            LOG("INTERNAL PROBLEM - ignoring unexpected event for"
                " domain %d (expected %d): event=%s",
                (*event_r)->domid, domid, evstr);
            free(evstr);
            libxl_event_free(ctx, *event_r);
            continue;
        }
        return ret;
    }
}

/*
 * Returns false if memory can't be freed, but also if we encounter errors.
 * Returns true in case there is already, or we manage to free it, enough
 * memory, but also if autoballoon is false.
 */
static bool freemem(uint32_t domid, libxl_domain_config *d_config)
{
    int rc;
    double credit = 30;
    uint64_t need_memkb, free_memkb;

    if (!autoballoon)
        return true;

    rc = libxl_domain_need_memory(ctx, d_config, domid, &need_memkb);
    if (rc < 0)
        return false;

    for (;;) {
        time_t start;

        rc = libxl_get_free_memory(ctx, &free_memkb);
        if (rc < 0)
            return false;

        if (free_memkb >= need_memkb)
            return true;

        if (credit <= 0)
            return false;

        rc = libxl_set_memory_target(ctx, 0, free_memkb - need_memkb, 1, 0);
        if (rc < 0)
            return false;

        /* wait until dom0 reaches its target, as long as we are making
         * progress */
        start = time(NULL);
        rc = libxl_wait_for_memory_target(ctx, 0, 10);
        if (rc < 0)
            return false;

        credit -= difftime(time(NULL), start);
    }
}

static void reload_domain_config(uint32_t domid,
                                 libxl_domain_config *d_config)
{
    int rc;
    uint8_t *t_data;
    int ret, t_len;
    libxl_domain_config d_config_new;

    /* In case user has used "config-update" to store a new config
     * file.
     */
    ret = libxl_userdata_retrieve(ctx, domid, "xl", &t_data, &t_len);
    if (ret && errno != ENOENT) {
        LOG("\"xl\" configuration found but failed to load\n");
    }
    if (t_len > 0) {
        LOG("\"xl\" configuration found, using it\n");
        libxl_domain_config_dispose(d_config);
        libxl_domain_config_init(d_config);
        parse_config_data("<updated>", (const char *)t_data,
                          t_len, d_config);
        free(t_data);
        libxl_userdata_unlink(ctx, domid, "xl");
        return;
    }

    libxl_domain_config_init(&d_config_new);
    rc = libxl_retrieve_domain_configuration(ctx, domid, &d_config_new,
                                             NULL);
    if (rc) {
        LOG("failed to retrieve guest configuration (rc=%d). "
            "reusing old configuration", rc);
        libxl_domain_config_dispose(&d_config_new);
    } else {
        libxl_domain_config_dispose(d_config);
        /* Steal allocations */
        memcpy(d_config, &d_config_new, sizeof(libxl_domain_config));
    }
}

/* Can update r_domid if domain is destroyed */
static domain_restart_type handle_domain_death(uint32_t *r_domid,
                                               libxl_event *event,
                                               libxl_domain_config *d_config)
{
    domain_restart_type restart = DOMAIN_RESTART_NONE;
    libxl_action_on_shutdown action;

    switch (event->u.domain_shutdown.shutdown_reason) {
    case LIBXL_SHUTDOWN_REASON_POWEROFF:
        action = d_config->on_poweroff;
        break;
    case LIBXL_SHUTDOWN_REASON_REBOOT:
        action = d_config->on_reboot;
        break;
    case LIBXL_SHUTDOWN_REASON_SUSPEND:
        LOG("Domain has suspended.");
        return 0;
    case LIBXL_SHUTDOWN_REASON_CRASH:
        action = d_config->on_crash;
        break;
    case LIBXL_SHUTDOWN_REASON_WATCHDOG:
        action = d_config->on_watchdog;
        break;
    case LIBXL_SHUTDOWN_REASON_SOFT_RESET:
        action = d_config->on_soft_reset;
        break;
    default:
        LOG("Unknown shutdown reason code %d. Destroying domain.",
            event->u.domain_shutdown.shutdown_reason);
        action = LIBXL_ACTION_ON_SHUTDOWN_DESTROY;
    }

    LOG("Action for shutdown reason code %d is %s",
        event->u.domain_shutdown.shutdown_reason,
        get_action_on_shutdown_name(action));

    if (action == LIBXL_ACTION_ON_SHUTDOWN_COREDUMP_DESTROY || action == LIBXL_ACTION_ON_SHUTDOWN_COREDUMP_RESTART) {
        char *corefile;
        int rc;

        xasprintf(&corefile, XEN_DUMP_DIR "/%s", d_config->c_info.name);
        LOG("dumping core to %s", corefile);
        rc = libxl_domain_core_dump(ctx, *r_domid, corefile, NULL);
        if (rc) LOG("core dump failed (rc=%d).", rc);
        free(corefile);
        /* No point crying over spilled milk, continue on failure. */

        if (action == LIBXL_ACTION_ON_SHUTDOWN_COREDUMP_DESTROY)
            action = LIBXL_ACTION_ON_SHUTDOWN_DESTROY;
        else
            action = LIBXL_ACTION_ON_SHUTDOWN_RESTART;
    }

    switch (action) {
    case LIBXL_ACTION_ON_SHUTDOWN_PRESERVE:
        break;

    case LIBXL_ACTION_ON_SHUTDOWN_RESTART_RENAME:
        reload_domain_config(*r_domid, d_config);
        restart = DOMAIN_RESTART_RENAME;
        break;

    case LIBXL_ACTION_ON_SHUTDOWN_RESTART:
        reload_domain_config(*r_domid, d_config);
        restart = DOMAIN_RESTART_NORMAL;
        /* fall-through */
    case LIBXL_ACTION_ON_SHUTDOWN_DESTROY:
        LOG("Domain %d needs to be cleaned up: destroying the domain",
            *r_domid);
        libxl_domain_destroy(ctx, *r_domid, 0);
        *r_domid = INVALID_DOMID;
        break;

    case LIBXL_ACTION_ON_SHUTDOWN_SOFT_RESET:
        reload_domain_config(*r_domid, d_config);
        restart = DOMAIN_RESTART_SOFT_RESET;
        break;

    case LIBXL_ACTION_ON_SHUTDOWN_COREDUMP_DESTROY:
    case LIBXL_ACTION_ON_SHUTDOWN_COREDUMP_RESTART:
        /* Already handled these above. */
        abort();
    }

    return restart;
}

/* Preserve a copy of a domain under a new name. Updates *r_domid */
static int preserve_domain(uint32_t *r_domid, libxl_event *event,
                           libxl_domain_config *d_config)
{
    time_t now;
    struct tm tm;
    char strtime[24];

    libxl_uuid new_uuid;

    int rc;

    now = time(NULL);
    if (now == ((time_t) -1)) {
        LOG("Failed to get current time for domain rename");
        return 0;
    }

    tzset();
    if (gmtime_r(&now, &tm) == NULL) {
        LOG("Failed to convert time to UTC");
        return 0;
    }

    if (!strftime(&strtime[0], sizeof(strtime), "-%Y%m%dT%H%MZ", &tm)) {
        LOG("Failed to format time as a string");
        return 0;
    }

    libxl_uuid_generate(&new_uuid);

    LOG("Preserving domain %u %s with suffix%s",
        *r_domid, d_config->c_info.name, strtime);
    rc = libxl_domain_preserve(ctx, *r_domid, &d_config->c_info,
                               strtime, new_uuid);

    /*
     * Although the domain still exists it is no longer the one we are
     * concerned with.
     */
    *r_domid = INVALID_DOMID;

    return rc == 0 ? 1 : 0;
}

static void console_child_report(xlchildnum child)
{
    if (xl_child_pid(child))
        child_report(child);
}

static int vncviewer(uint32_t domid, int autopass)
{
    libxl_vncviewer_exec(ctx, domid, autopass);
    fprintf(stderr, "Unable to execute vncviewer\n");
    return 1;
}

static void autoconnect_vncviewer(uint32_t domid, int autopass)
{
   console_child_report(child_vncviewer);

    pid_t pid = xl_fork(child_vncviewer, "vncviewer child");
    if (pid)
        return;

    postfork();

    sleep(1);
    vncviewer(domid, autopass);
    _exit(EXIT_FAILURE);
}

static int acquire_lock(void)
{
    int rc;
    struct flock fl;

    /* lock already acquired */
    if (fd_lock >= 0)
        return ERROR_INVAL;

    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    fd_lock = open(lockfile, O_WRONLY|O_CREAT, S_IWUSR);
    if (fd_lock < 0) {
        fprintf(stderr, "cannot open the lockfile %s errno=%d\n", lockfile, errno);
        return ERROR_FAIL;
    }
    if (fcntl(fd_lock, F_SETFD, FD_CLOEXEC) < 0) {
        close(fd_lock);
        fprintf(stderr, "cannot set cloexec to lockfile %s errno=%d\n", lockfile, errno);
        return ERROR_FAIL;
    }
get_lock:
    rc = fcntl(fd_lock, F_SETLKW, &fl);
    if (rc < 0 && errno == EINTR)
        goto get_lock;
    if (rc < 0) {
        fprintf(stderr, "cannot acquire lock %s errno=%d\n", lockfile, errno);
        rc = ERROR_FAIL;
    } else
        rc = 0;
    return rc;
}

static int release_lock(void)
{
    int rc;
    struct flock fl;

    /* lock not acquired */
    if (fd_lock < 0)
        return ERROR_INVAL;

release_lock:
    fl.l_type = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;

    rc = fcntl(fd_lock, F_SETLKW, &fl);
    if (rc < 0 && errno == EINTR)
        goto release_lock;
    if (rc < 0) {
        fprintf(stderr, "cannot release lock %s, errno=%d\n", lockfile, errno);
        rc = ERROR_FAIL;
    } else
        rc = 0;
    close(fd_lock);
    fd_lock = -1;

    return rc;
}


static void autoconnect_console(libxl_ctx *ctx_ignored,
                                libxl_event *ev, void *priv)
{
    uint32_t bldomid = ev->domid;
    int notify_fd = *(int*)priv; /* write end of the notification pipe */

    libxl_event_free(ctx, ev);

    console_child_report(child_console);

    pid_t pid = xl_fork(child_console, "console child");
    if (pid)
        return;

    postfork();

    sleep(1);
    libxl_primary_console_exec(ctx, bldomid, notify_fd);
    /* Do not return. xl continued in child process */
    perror("xl: unable to exec console client");
    _exit(1);
}

int create_domain(struct domain_create *dom_info)
{
    uint32_t domid = INVALID_DOMID;

    libxl_domain_config d_config;

    int debug = dom_info->debug;
    int daemonize = dom_info->daemonize;
    int monitor = dom_info->monitor;
    int paused = dom_info->paused;
    int vncautopass = dom_info->vncautopass;
    const char *config_file = dom_info->config_file;
    const char *extra_config = dom_info->extra_config;
    const char *restore_file = dom_info->restore_file;
    const char *config_source = NULL;
    const char *restore_source = NULL;
    int migrate_fd = dom_info->migrate_fd;
    bool config_in_json;

    int i;
    int need_daemon = daemonize;
    int ret, rc;
    libxl_evgen_domain_death *deathw = NULL;
    libxl_evgen_disk_eject **diskws = NULL; /* one per disk */
    unsigned int num_diskws = 0;
    void *config_data = 0;
    int config_len = 0;
    int restore_fd = -1;
    int restore_fd_to_close = -1;
    int send_back_fd = -1;
    const libxl_asyncprogress_how *autoconnect_console_how;
    int notify_pipe[2] = { -1, -1 };
    struct save_file_header hdr;
    uint32_t domid_soft_reset = INVALID_DOMID;

    int restoring = (restore_file || (migrate_fd >= 0));

    libxl_domain_config_init(&d_config);

    if (restoring) {
        uint8_t *optdata_begin = 0;
        const uint8_t *optdata_here = 0;
        union { uint32_t u32; char b[4]; } u32buf;
        uint32_t badflags;

        if (migrate_fd >= 0) {
            restore_source = "<incoming migration stream>";
            restore_fd = migrate_fd;
            send_back_fd = dom_info->send_back_fd;
        } else {
            restore_source = restore_file;
            restore_fd = open(restore_file, O_RDONLY);
            if (restore_fd == -1) {
                fprintf(stderr, "Can't open restore file: %s\n", strerror(errno));
                return ERROR_INVAL;
            }
            restore_fd_to_close = restore_fd;
            rc = libxl_fd_set_cloexec(ctx, restore_fd, 1);
            if (rc) return rc;
        }

        CHK_ERRNOVAL(libxl_read_exactly(
                         ctx, restore_fd, &hdr, sizeof(hdr),
                         restore_source, "header"));
        if (memcmp(hdr.magic, savefileheader_magic, sizeof(hdr.magic))) {
            fprintf(stderr, "File has wrong magic number -"
                    " corrupt or for a different tool?\n");
            return ERROR_INVAL;
        }
        if (hdr.byteorder != SAVEFILE_BYTEORDER_VALUE) {
            fprintf(stderr, "File has wrong byte order\n");
            return ERROR_INVAL;
        }
        fprintf(stderr, "Loading new save file %s"
                " (new xl fmt info"
                " 0x%"PRIx32"/0x%"PRIx32"/%"PRIu32")\n",
                restore_source, hdr.mandatory_flags, hdr.optional_flags,
                hdr.optional_data_len);

        badflags = hdr.mandatory_flags & ~XL_MANDATORY_FLAG_ALL;
        if (badflags) {
            fprintf(stderr, "Savefile has mandatory flag(s) 0x%"PRIx32" "
                    "which are not supported; need newer xl\n",
                    badflags);
            return ERROR_INVAL;
        }
        if (hdr.optional_data_len) {
            optdata_begin = xmalloc(hdr.optional_data_len);
            CHK_ERRNOVAL(libxl_read_exactly(
                             ctx, restore_fd, optdata_begin,
                             hdr.optional_data_len, restore_source,
                             "optdata"));
        }

#define OPTDATA_LEFT  (hdr.optional_data_len - (optdata_here - optdata_begin))
#define WITH_OPTDATA(amt, body)                                 \
            if (OPTDATA_LEFT < (amt)) {                         \
                fprintf(stderr, "Savefile truncated.\n");       \
                return ERROR_INVAL;                             \
            } else {                                            \
                body;                                           \
                optdata_here += (amt);                          \
            }

        optdata_here = optdata_begin;

        if (OPTDATA_LEFT) {
            fprintf(stderr, " Savefile contains xl domain config%s\n",
                    !!(hdr.mandatory_flags & XL_MANDATORY_FLAG_JSON)
                    ? " in JSON format" : "");
            WITH_OPTDATA(4, {
                memcpy(u32buf.b, optdata_here, 4);
                config_len = u32buf.u32;
            });
            WITH_OPTDATA(config_len, {
                config_data = xmalloc(config_len);
                memcpy(config_data, optdata_here, config_len);
            });
        }

    }

    if (config_file) {
        free(config_data);  config_data = 0;
        /* /dev/null represents special case (read config. from command line) */
        if (!strcmp(config_file, "/dev/null")) {
            config_len = 0;
        } else {
            ret = libxl_read_file_contents(ctx, config_file,
                                           &config_data, &config_len);
            if (ret) { fprintf(stderr, "Failed to read config file: %s: %s\n",
                               config_file, strerror(errno)); return ERROR_FAIL; }
        }
        if (!restoring && extra_config && strlen(extra_config)) {
            if (config_len > INT_MAX - (strlen(extra_config) + 2 + 1)) {
                fprintf(stderr, "Failed to attach extra configuration\n");
                return ERROR_FAIL;
            }
            /* allocate space for the extra config plus two EOLs plus \0 */
            config_data = xrealloc(config_data, config_len
                + strlen(extra_config) + 2 + 1);
            config_len += sprintf(config_data + config_len, "\n%s\n",
                extra_config);
        }
        config_source=config_file;
        config_in_json = false;
    } else {
        if (!config_data) {
            fprintf(stderr, "Config file not specified and"
                    " none in save file\n");
            return ERROR_INVAL;
        }
        config_source = "<saved>";
        config_in_json = !!(hdr.mandatory_flags & XL_MANDATORY_FLAG_JSON);
    }

    if (!dom_info->quiet)
        fprintf(stderr, "Parsing config from %s\n", config_source);

    if (config_in_json) {
        libxl_domain_config_from_json(ctx, &d_config,
                                      (const char *)config_data);
    } else {
        parse_config_data(config_source, config_data, config_len, &d_config);
    }

    if (!dom_info->ignore_global_affinity_masks) {
        libxl_domain_build_info *b_info = &d_config.b_info;

        /* It is possible that no hard affinity is specified in config file.
         * Generate hard affinity maps now if we care about those.
         */
        if (b_info->num_vcpu_hard_affinity == 0 &&
              (!libxl_bitmap_is_full(&global_vm_affinity_mask) ||
                 (b_info->type == LIBXL_DOMAIN_TYPE_PV &&
                  !libxl_bitmap_is_full(&global_pv_affinity_mask)) ||
                 (b_info->type != LIBXL_DOMAIN_TYPE_PV &&
                  !libxl_bitmap_is_full(&global_hvm_affinity_mask))
               )) {
            b_info->num_vcpu_hard_affinity = b_info->max_vcpus;
            b_info->vcpu_hard_affinity =
                xmalloc(b_info->max_vcpus * sizeof(libxl_bitmap));

            for (i = 0; i < b_info->num_vcpu_hard_affinity; i++) {
                libxl_bitmap *m = &b_info->vcpu_hard_affinity[i];
                libxl_bitmap_init(m);
                libxl_cpu_bitmap_alloc(ctx, m, 0);
                libxl_bitmap_set_any(m);
            }
        }

        apply_global_affinity_masks(b_info->type,
                                    b_info->vcpu_hard_affinity,
                                    b_info->num_vcpu_hard_affinity);
    }

    if (migrate_fd >= 0) {
        if (d_config.c_info.name) {
            /* when we receive a domain we get its name from the config
             * file; and we receive it to a temporary name */
            assert(!common_domname);

            common_domname = d_config.c_info.name;
            d_config.c_info.name = 0; /* steals allocation from config */

            xasprintf(&d_config.c_info.name, "%s--incoming", common_domname);
            *dom_info->migration_domname_r = strdup(d_config.c_info.name);
        }
    }

    if (debug || dryrun_only) {
        FILE *cfg_print_fh = (debug && !dryrun_only) ? stderr : stdout;
        if (default_output_format == OUTPUT_FORMAT_SXP) {
            printf_info_sexp(-1, &d_config, cfg_print_fh);
        } else {
            char *json = libxl_domain_config_to_json(ctx, &d_config);
            if (!json) {
                fprintf(stderr,
                        "Failed to convert domain configuration to JSON\n");
                exit(1);
            }
            fputs(json, cfg_print_fh);
            free(json);
            flush_stream(cfg_print_fh);
        }
    }


    ret = 0;
    if (dryrun_only)
        goto out;

start:
    assert(domid == INVALID_DOMID);

    if (autoballoon) {
        rc = acquire_lock();
        if (rc < 0)
            goto error_out;
    }

    if (domid_soft_reset == INVALID_DOMID) {
        if (!freemem(domid, &d_config)) {
            fprintf(stderr, "failed to free memory for the domain\n");
            ret = ERROR_FAIL;
            goto error_out;
        }
    }

    libxl_asyncprogress_how autoconnect_console_how_buf;
    if ( dom_info->console_autoconnect ) {
        if (libxl_pipe(ctx, notify_pipe)) {
            ret = ERROR_FAIL;
            goto error_out;
        }
        autoconnect_console_how_buf.callback = autoconnect_console;
        autoconnect_console_how_buf.for_callback = &notify_pipe[1];
        autoconnect_console_how = &autoconnect_console_how_buf;
    }else{
        autoconnect_console_how = 0;
    }

    if (!libxl_domid_valid_guest(d_config.c_info.domid))
        d_config.c_info.domid = domid_policy;

    if ( restoring ) {
        libxl_domain_restore_params params;

        libxl_domain_restore_params_init(&params);

        params.checkpointed_stream = dom_info->checkpointed_stream;
        params.stream_version =
            (hdr.mandatory_flags & XL_MANDATORY_FLAG_STREAMv2) ? 2 : 1;
        params.colo_proxy_script = dom_info->colo_proxy_script;
        libxl_defbool_set(&params.userspace_colo_proxy,
                          dom_info->userspace_colo_proxy);

        ret = libxl_domain_create_restore(ctx, &d_config,
                                          &domid, restore_fd,
                                          send_back_fd, &params,
                                          0, autoconnect_console_how);

        libxl_domain_restore_params_dispose(&params);

        /*
         * On subsequent reboot etc we should create the domain, not
         * restore/migrate-receive it again.
         */
        restoring = 0;
    } else if (domid_soft_reset != INVALID_DOMID) {
        /* Do soft reset. */
        ret = libxl_domain_soft_reset(ctx, &d_config, domid_soft_reset,
                                      0, autoconnect_console_how);
        domid = domid_soft_reset;
        domid_soft_reset = INVALID_DOMID;
    } else {
        ret = libxl_domain_create_new(ctx, &d_config, &domid,
                                      0, autoconnect_console_how);
    }
    if ( ret )
        goto error_out;

    if (autoballoon)
        release_lock();

    if (restore_fd_to_close >= 0) {
        if (close(restore_fd_to_close))
            fprintf(stderr, "Failed to close restoring file, fd %d, errno %d\n",
                    restore_fd_to_close, errno);
        restore_fd_to_close = -1;
    }

    if (autoconnect_console_how) {
        char buf[1];
        int r;

        /* Try to get notification from xenconsole. Just move on if
         * error occurs -- it's only minor annoyance if console
         * doesn't show up.
         */
        do {
            r = read(notify_pipe[0], buf, 1);
        } while (r == -1 && errno == EINTR);

        if (r == -1)
            fprintf(stderr,
                    "Failed to get notification from xenconsole: %s\n",
                    strerror(errno));
        else if (r == 0)
            fprintf(stderr, "Got EOF from xenconsole notification fd\n");
        else if (r == 1 && buf[0] != 0x00)
            fprintf(stderr, "Got unexpected response from xenconsole: %#x\n",
                    buf[0]);

        close(notify_pipe[0]);
        close(notify_pipe[1]);
        notify_pipe[0] = notify_pipe[1] = -1;
    }

    if (!paused)
        libxl_domain_unpause(ctx, domid, NULL);

    ret = domid; /* caller gets success in parent */
    if (!daemonize && !monitor)
        goto out;

    if (dom_info->vnc)
        autoconnect_vncviewer(domid, vncautopass);

    if (need_daemon) {
        char *name;

        xasprintf(&name, "xl-%s", d_config.c_info.name);
        ret = do_daemonize(name, NULL);
        free(name);
        if (ret) {
            ret = (ret == 1) ? domid : ret;
            goto out;
        }
        need_daemon = 0;
    }
    LOG("Waiting for domain %s (domid %u) to die [pid %ld]",
        d_config.c_info.name, domid, (long)getpid());

    ret = libxl_evenable_domain_death(ctx, domid, 0, &deathw);
    if (ret) goto out;

    if (!diskws) {
        diskws = xmalloc(sizeof(*diskws) * d_config.num_disks);
        for (i = 0; i < d_config.num_disks; i++)
            diskws[i] = NULL;
        num_diskws = d_config.num_disks;
    }
    for (i = 0; i < num_diskws; i++) {
        if (d_config.disks[i].removable) {
            ret = libxl_evenable_disk_eject(ctx, domid, d_config.disks[i].vdev,
                                            0, &diskws[i]);
            if (ret) goto out;
        }
    }
    while (1) {
        libxl_event *event;
        ret = domain_wait_event(domid, &event);
        if (ret) goto out;

        switch (event->type) {

        case LIBXL_EVENT_TYPE_DOMAIN_SHUTDOWN:
            LOG("Domain %u has shut down, reason code %d 0x%x", domid,
                event->u.domain_shutdown.shutdown_reason,
                event->u.domain_shutdown.shutdown_reason);
            switch (handle_domain_death(&domid, event, &d_config)) {
            case DOMAIN_RESTART_SOFT_RESET:
                domid_soft_reset = domid;
                domid = INVALID_DOMID;
                /* fall through */
            case DOMAIN_RESTART_RENAME:
                if (domid_soft_reset == INVALID_DOMID &&
                    !preserve_domain(&domid, event, &d_config)) {
                    libxl_event_free(ctx, event);
                    /* If we fail then exit leaving the old domain in place. */
                    ret = -1;
                    goto out;
                }

                /* Otherwise fall through and restart. */
            case DOMAIN_RESTART_NORMAL:
                libxl_event_free(ctx, event);
                libxl_evdisable_domain_death(ctx, deathw);
                deathw = NULL;
                evdisable_disk_ejects(diskws, num_diskws);
                free(diskws);
                diskws = NULL;
                num_diskws = 0;
                /* discard any other events which may have been generated */
                while (!(ret = libxl_event_check(ctx, &event,
                                                 LIBXL_EVENTMASK_ALL, 0,0))) {
                    libxl_event_free(ctx, event);
                }
                if (ret != ERROR_NOT_READY) {
                    LOG("warning, libxl_event_check (cleanup) failed (rc=%d)",
                        ret);
                }

                /*
                 * Do not attempt to reconnect if we come round again due to a
                 * guest reboot -- the stdin/out will be disconnected by then.
                 */
                dom_info->console_autoconnect = 0;

                /* Some settings only make sense on first boot. */
                paused = 0;
                if (common_domname
                    && strcmp(d_config.c_info.name, common_domname)) {
                    d_config.c_info.name = strdup(common_domname);
                }

                /*
                 * XXX FIXME: If this sleep is not there then domain
                 * re-creation fails sometimes.
                 */
                LOG("Done. Rebooting now");
                sleep(2);
                goto start;

            case DOMAIN_RESTART_NONE:
                LOG("Done. Exiting now");
                libxl_event_free(ctx, event);
                ret = 0;
                goto out;

            default:
                abort();
            }

        case LIBXL_EVENT_TYPE_DOMAIN_DEATH:
            LOG("Domain %u has been destroyed.", domid);
            libxl_event_free(ctx, event);
            ret = 0;
            goto out;

        case LIBXL_EVENT_TYPE_DISK_EJECT:
            /* XXX what is this for? */
            libxl_cdrom_insert(ctx, domid, &event->u.disk_eject.disk, NULL);
            break;

        default:;
            char *evstr = libxl_event_to_json(ctx, event);
            LOG("warning, got unexpected event type %d, event=%s",
                event->type, evstr);
            free(evstr);
        }

        libxl_event_free(ctx, event);
    }

error_out:
    if (autoballoon)
        release_lock();
    if (libxl_domid_valid_guest(domid)) {
        libxl_domain_destroy(ctx, domid, 0);
        domid = INVALID_DOMID;
    }

out:
    if (restore_fd_to_close >= 0) {
        if (close(restore_fd_to_close))
            fprintf(stderr, "Failed to close restoring file, fd %d, errno %d\n",
                    restore_fd_to_close, errno);
        restore_fd_to_close = -1;
    }

    if (logfile != 2)
        close(logfile);

    libxl_domain_config_dispose(&d_config);

    free(config_data);

    console_child_report(child_console);

    if (deathw)
        libxl_evdisable_domain_death(ctx, deathw);
    if (diskws) {
        evdisable_disk_ejects(diskws, d_config.num_disks);
        free(diskws);
    }

    /*
     * If we have daemonized then do not return to the caller -- this has
     * already happened in the parent.
     */
    if ( daemonize && !need_daemon )
        exit(ret);

    return ret;
}

int main_create(int argc, char **argv)
{
    struct domain_create dom_info = {
        /* Command-line options */
        .config_file = NULL,
        .console_autoconnect = 0,
        .debug = 0,
        .daemonize = 1,
        .ignore_global_affinity_masks = 0,
        .monitor = 1,
        .paused = 0,
        .quiet = 0,
        .vnc = 0,
        .vncautopass = 0,

        /* Extra configuration file settings */
        .extra_config = NULL,

        /* FDs, initialize to invalid */
        .migrate_fd = -1,
        .send_back_fd = -1,
    };
    int opt, rc;
    static const struct option opts[] = {
        {"defconfig", 1, 0, 'f'},
        {"dryrun", 0, 0, 'n'},
        {"ignore-global-affinity-masks", 0, 0, 'i'},
        {"quiet", 0, 0, 'q'},
        {"vncviewer", 0, 0, 'V'},
        {"vncviewer-autopass", 0, 0, 'A'},
        COMMON_LONG_OPTS
    };

    if (argv[1] && argv[1][0] != '-' && !strchr(argv[1], '=')) {
        dom_info.config_file = argv[1];
        argc--; argv++;
    }

    SWITCH_FOREACH_OPT(opt, "AFVcdef:inpq", opts, "create", 0) {
    case 'A':
        dom_info.vnc = dom_info.vncautopass = 1;
        break;
    case 'F':
        dom_info.daemonize = 0;
        break;
    case 'V':
        dom_info.vnc = 1;
        break;
    case 'c':
        dom_info.console_autoconnect = 1;
        break;
    case 'd':
        dom_info.debug = 1;
        break;
    case 'e':
        dom_info.daemonize = 0;
        dom_info.monitor = 0;
        break;
    case 'f':
        dom_info.config_file = optarg;
        break;
    case 'i':
        dom_info.ignore_global_affinity_masks = 1;
        break;
    case 'n':
        dryrun_only = 1;
        break;
    case 'p':
        dom_info.paused = 1;
        break;
    case 'q':
        dom_info.quiet = 1;
        break;
    }

    for (; optind < argc; optind++) {
        if (strchr(argv[optind], '=') != NULL) {
            string_realloc_append(&dom_info.extra_config, argv[optind]);
            string_realloc_append(&dom_info.extra_config, "\n");
        } else if (!dom_info.config_file) {
            dom_info.config_file = argv[optind];
        } else {
            help("create");
            free(dom_info.extra_config);
            return 2;
        }
    }

    rc = create_domain(&dom_info);
    if (rc < 0) {
        free(dom_info.extra_config);
        return -rc;
    }

    free(dom_info.extra_config);
    return 0;
}

static int copy_overlay_to_domU(uint32_t domain_id, void *overlay_dt_domU,
                                uint32_t overlay_dt_size, uint32_t *grant_ref,
                                uint32_t num_pages)
{
    void *buffer = NULL;
    xengnttab_handle *gnttab = xengnttab_open(NULL, 0);

    if (!gnttab) {
        fprintf(stderr,"opening gnttab failed for domain %d\n", domain_id);
        return -1;
    }

    buffer = xengnttab_map_domain_grant_refs(gnttab, num_pages, domain_id,
                                             grant_ref, PROT_READ|PROT_WRITE);

    if (buffer == NULL) {
        fprintf(stderr, "Getting the buffer failed for grant_refs\n");

        return -1;
    }

    /* buffer is contiguous allocated. */
    memcpy(buffer, overlay_dt_domU, overlay_dt_size);

    xengnttab_unmap(gnttab, buffer, num_pages);

    return 0;
}

static bool wait_for_status(struct xs_handle *xs, int fd, char *status_path,
                            const char *status)
{
    unsigned int num_strings;
    char *buf = NULL;
    char **vec = NULL;
    bool ret = false;
    unsigned int len;
    int rc = 0;
    fd_set set;

    while (1)
    {
        FD_ZERO(&set);
        FD_SET(fd, &set);

        rc = select(fd + 1, &set, NULL, NULL, NULL);
        /* Poll for data: Blocking. */
        if (rc <= 0)
            break;

        if (FD_ISSET(fd, &set)) {
            /*
             * num_strings will be set to the number of elements in vec
             * (2 - the watched path and the overlay_watch)
             */
            vec = xs_read_watch(xs, &num_strings);
            if (!vec) {
                break;
            }

            /* do a read. */
            buf = xs_read(xs, XBT_NULL, status_path, &len);
            if (buf) {
                if (!strcmp(buf, status)) {
                    ret = true;
                    break;
                }
            }
        }
    }

    free(vec);
    free(buf);

    return ret;
}

static bool write_overlay_size(struct xs_handle *xs, uint32_t overlay_size,
                               char *path)
{
    xs_transaction_t xs_trans = XBT_NULL;
    char buf[128];
    char ref[16];

retry_transaction:
    xs_trans = xs_transaction_start(xs);
    if (!xs_trans)
        return false;

    snprintf(ref, sizeof(ref), "%u", overlay_size);
    snprintf(buf, sizeof(buf), "%s/overlay-size", path);

    if (!xs_write(xs, xs_trans, buf, ref, strlen(ref)))
        return false;

    if (!xs_transaction_end(xs, xs_trans, 0)) {
        if (errno == EAGAIN)
            goto retry_transaction;
        else
            return false;
    }

    return true;
}

static bool write_status(struct xs_handle *xs, char *status, char *status_path)
{
    xs_transaction_t xs_trans = XBT_NULL;

retry_transaction:
    xs_trans = xs_transaction_start(xs);
    if (!xs_trans)
        return false;

    if (!xs_write(xs, xs_trans, status_path, status, strlen(status)))
        return false;

    if (!xs_transaction_end(xs, xs_trans, 0)) {
        if (errno == EAGAIN)
            goto retry_transaction;
        else
            return false;
    }

    return true;
}

static uint32_t *get_page_ref(struct xs_handle *xs, const char *xs_path,
                              uint32_t num_pages)
{
    char buf[128];
    uint32_t *ref = NULL;
    char *data;
    char temp[16] = { };
    unsigned int len;
    int i = 0;
    int j = 0;
    int start_ind = 0;

    snprintf(buf, sizeof(buf), "%s/page-ref", xs_path);

    /* do a read. */
    data = xs_read(xs, XBT_NULL, buf, &len);

    if (data) {
        /* Caller will free this. */
        ref = (uint32_t *)calloc(num_pages, sizeof(uint32_t));

        for (i = 0; data[i] != '\0'; i++) {
            if (data[i] == ',') {
                /* Next page_ref data. */
                memset(temp, '\0', sizeof(temp));

                /* Copy the data between previous ',' to current. */
                memcpy(temp, &data[start_ind], i - start_ind);

                /* Convert to int. */
                ref[j] = atoi(temp);
                start_ind = i + 1;
                j++;
            }

            if (j == num_pages)
                break;
        }
    }

    if (j != num_pages) {
        fprintf(stderr, "Number of page_refs are not equal to num_pages\n");
        free(ref);
        ref = NULL;
    }

    free(data);
    return ref;
}

static uint32_t get_num_pages(struct xs_handle *xs, const char *xs_path)
{
    char buf[128];
    char *ref;
    unsigned int len;
    uint32_t num_pages = 0;

    snprintf(buf, sizeof(buf), "%s/num-pages", xs_path);

    /* do a read. */
    ref = xs_read(xs, XBT_NULL, buf, &len);

    if (ref)
        num_pages = atoi(ref);

    free(ref);

    return num_pages;
}

static bool write_overlay_operation(struct xs_handle *xs, char *operation,
                               char *path)
{
    xs_transaction_t xs_trans = XBT_NULL;
    char buf[128];
    char ref[64];

retry_transaction:
    xs_trans = xs_transaction_start(xs);
    if (!xs_trans)
        return false;

    snprintf(ref, sizeof(ref), "%s", operation);
    snprintf(buf, sizeof(buf), "%s/overlay-operation", path);

    if (!xs_write(xs, xs_trans, buf, ref, strlen(ref)))
        return false;

    if (!xs_transaction_end(xs, xs_trans, 0)) {
        if (errno == EAGAIN)
            goto retry_transaction;
        else
            return false;
    }

    return true;
}

static bool write_overlay_name(struct xs_handle *xs, char *name,
                               char *path)
{
    xs_transaction_t xs_trans = XBT_NULL;
    char buf[128];
    char ref[64];

retry_transaction:
    xs_trans = xs_transaction_start(xs);
    if (!xs_trans)
        return false;

    snprintf(ref, sizeof(ref), "%s", name);
    snprintf(buf, sizeof(buf), "%s/overlay-name", path);

    if (!xs_write(xs, xs_trans, buf, ref, strlen(ref)))
        return false;

    if (!xs_transaction_end(xs, xs_trans, 0)) {
        if (errno == EAGAIN)
            goto retry_transaction;
        else
            return false;
    }

    return true;
}

static bool write_overlay_type(struct xs_handle *xs, char *type,
                               char *path)
{
    xs_transaction_t xs_trans = XBT_NULL;
    char buf[128];
    char ref[64];

retry_transaction:
    xs_trans = xs_transaction_start(xs);
    if (!xs_trans)
        return false;

    snprintf(ref, sizeof(ref), "%s", type);
    snprintf(buf, sizeof(buf), "%s/overlay-type", path);

    if (!xs_write(xs, xs_trans, buf, ref, strlen(ref)))
        return false;

    if (!xs_transaction_end(xs, xs_trans, 0)) {
        if (errno == EAGAIN)
            goto retry_transaction;
        else
            return false;
    }

    return true;
}

static bool write_overlay_partial(struct xs_handle *xs, bool is_partial,
                                  char *path)
{
    xs_transaction_t xs_trans = XBT_NULL;
    char buf[128];
    char ref[4];

retry_transaction:
    xs_trans = xs_transaction_start(xs);
    if (!xs_trans)
        return false;

    snprintf(ref, sizeof(ref), "%d", is_partial);
    snprintf(buf, sizeof(buf), "%s/overlay-partial", path);

    if (!xs_write(xs, xs_trans, buf, ref, strlen(ref)))
        return false;

    if (!xs_transaction_end(xs, xs_trans, 0)) {
        if (errno == EAGAIN)
            goto retry_transaction;
        else
            return false;
    }

    return true;
}


static int share_overlay_with_domu(void *overlay_dt_domU, int overlay_dt_size,
                                   int domain_id, char *overlay_ops,
                                   char *overlay_name,
                                   char *overlay_type, bool is_overlay_partial)
{
    struct xs_handle *xs = NULL;
    char *path = NULL;
    char receiver_status_path[64] = { };
    char sender_status_path[64] = { };
    int fd = 0;
    int err;
    uint32_t num_pages= 1;
    uint32_t *page_ref = NULL;

    /* XS_watch part. */
    /* Open a connection to the xenstore. */
    xs = xs_open(0);
    if (xs == NULL) {
        fprintf(stderr, "Deamon open for domain%d failed\n", domain_id);
        err = ERROR_FAIL;
        goto out;
    }

    /* Get the local domain path */
    path = xs_get_domain_path(xs, domain_id);
    if (path == NULL) {
        fprintf(stderr, "Getting domain%d path failed\n", domain_id);
        err = ERROR_FAIL;
        goto out;
    }

    /* Make space for our node on the path */
    path = realloc(path, strlen(path) + strlen("/data/overlay") + 1);
    if (path == NULL) {
        fprintf(stderr, "Path allocation failed\n");
        err = ERROR_NOMEM;
        goto out;
    }

    strcat(path, "/data/overlay");
    strcpy(receiver_status_path, path);
    strcat(receiver_status_path, "/receiver-status");

    /*
     * Watch a node for changes (poll on fd to detect).
     * When the node (or any child) changes, fd will become readable.
     */
    err = xs_watch(xs, receiver_status_path, "overlay_watch");
    if (!err) {
        fprintf(stderr, "Creating watch failed\n");
        err = ERROR_FAIL;
        goto out;
    }

    /*
     * We are notified of read availability on the watch via the
     * file descriptor.
     */
    fd = xs_fileno(xs);

    /* Wait for "waiting" status from other domain. */
    if (!wait_for_status(xs, fd, receiver_status_path, "waiting")) {
        err = ERROR_NOT_READY;
        goto out;
    }

    /* Share the dtb size with domain. */
    if (!write_overlay_size(xs, overlay_dt_size, path)) {
        err = ERROR_FAIL;
        fprintf(stderr,"Writing page ref failed\n");
        goto out;
    }

    strcpy(sender_status_path, path);
    strcat(sender_status_path, "/sender-status");

    /* Write the status "ready". */
    if (!write_status(xs, "ready", sender_status_path)) {
        err = ERROR_FAIL;
        fprintf(stderr,"Writing status ready failed\n");
        goto out;
    }

    /* Wait for "page_ref" status from other domain. */
    if (!wait_for_status(xs, fd, receiver_status_path, "page_ref")) {
        err = ERROR_NOT_READY;
        goto out;
    }

    num_pages = get_num_pages(xs, path);
    if (num_pages == 0) {
        fprintf(stderr, "no pages allocated\n");
        err = ERROR_FAIL;
        goto out;
    }

    page_ref = get_page_ref(xs, path, num_pages);
    if (page_ref == NULL) {
        fprintf(stderr,"page ref is null.\n");
        err = ERROR_FAIL;
        goto out;
    }

    if (copy_overlay_to_domU(domain_id, overlay_dt_domU, overlay_dt_size,
                             page_ref, num_pages)) {
        fprintf(stderr,"Copy overlay failed\n");
        err = ERROR_FAIL;
        goto out;
    }

    /* write overlay ops */
    if (!write_overlay_operation(xs, overlay_ops, path)) {
        err = ERROR_FAIL;
        fprintf(stderr,"Writing overlay_ops ready failed\n");
        goto out;
    }

    /* Write the overlay-name. */
    if (!write_overlay_name(xs, overlay_name, path)) {
        err = ERROR_FAIL;
        fprintf(stderr,"Writing overlay_name ready failed\n");
        goto out;
    }

    /* Write the overlay-type. */
    if (!write_overlay_type(xs, overlay_type, path)) {
        err = ERROR_FAIL;
        fprintf(stderr,"Writing overlay_type ready failed\n");
        goto out;
    }

    /* Write the overlay-partial. */
    if (!write_overlay_partial(xs, is_overlay_partial, path)) {
        err = ERROR_FAIL;
        fprintf(stderr,"Writing overlay_partial ready failed\n");
        goto out;
    }

    /* Write the status "done". */
    if (!write_status(xs, "done", sender_status_path)) {
        fprintf(stderr,"Writing status DONE failed\n");
        err = ERROR_FAIL;
        goto out;
    }

/* Cleanup */
out:
    if (xs) {
        close(fd);
        xs_unwatch(xs, path, "overlay_watch");
        xs_close(xs);
    }

    if (path)
        free(path);

    if (page_ref)
        free(page_ref);

    return err;
}

int main_dt_overlay(int argc, char **argv)
{
    const char *overlay_ops;
    uint32_t domain_id = 0;
    const char *overlay_config_file;
    void *overlay_dtb = NULL;
    int rc;
    bool auto_mode = true;
    bool domain_mapping = false;
    uint8_t op;
    int overlay_dtb_size = 0;
    char *overlay_name = "overlay";
    char *overlay_type = "normal";
    bool is_overlay_partial = false;

    if (argc < 3) {
        fprintf(stderr, "Not enough arguments\n");
        return ERROR_FAIL;
    }

    if (argc > 7) {
        fprintf(stderr, "Too many arguments\n");
        return ERROR_FAIL;
    }

    overlay_ops = argv[1];
    overlay_config_file = argv[2];

    if (argc == 4 ) {
        domain_id = find_domain(argv[argc-1]);
        domain_mapping = true;
    } else if (argc == 5 && !strcmp(argv[4], "-e")) {
        domain_id = find_domain(argv[3]);
        auto_mode = false;
        domain_mapping = true;
    } else if (argc == 7) {
        domain_id = find_domain(argv[3]);
        domain_mapping = true;
        overlay_name = argv[4];
        overlay_type = argv[5];
        is_overlay_partial = atoi(argv[6]);
    } else {
        fprintf(stderr, "Invalid arguments\n");
        return ERROR_FAIL;
    }

    /* User didn't prove any overlay operation. */
    if (overlay_ops == NULL) {
        fprintf(stderr, "No overlay operation mode provided\n");
        return ERROR_FAIL;
    }

    if (strcmp(overlay_ops, "add") == 0)
        op = XL_DT_OVERLAY_ADD;
    else if (strcmp(overlay_ops, "remove") == 0)
        op = XL_DT_OVERLAY_REMOVE;
    else {
        fprintf(stderr, "Invalid dt overlay operation\n");
        return ERROR_FAIL;
    }

    if (overlay_config_file) {
        rc = libxl_read_file_contents(ctx, overlay_config_file,
                                      &overlay_dtb, &overlay_dtb_size);

        if (rc) {
            fprintf(stderr, "failed to read the overlay device tree file %s\n",
                    overlay_config_file);
            free(overlay_dtb);
            return ERROR_FAIL;
        }
    } else {
        fprintf(stderr, "overlay dtbo file not provided\n");
        return ERROR_FAIL;
    }

    rc = libxl_dt_overlay(ctx, domain_id, overlay_dtb, overlay_dtb_size, op,
                          auto_mode, domain_mapping);
    if (rc) {
        fprintf(stderr, "Overlay operation failed\n");
        goto out;
    }

    if (domain_id && auto_mode && (op == LIBXL_DT_OVERLAY_ADD)) {

        rc = share_overlay_with_domu(overlay_dtb, overlay_dtb_size,
                                     domain_id, "add", overlay_name,
                                     overlay_type, is_overlay_partial);
        if (rc)
            goto out;
    }

    if (domain_id && auto_mode && (op == LIBXL_DT_OVERLAY_REMOVE)) {

        rc = share_overlay_with_domu(overlay_dtb, overlay_dtb_size,
                                     domain_id, "remove", overlay_name,
                                     overlay_type, is_overlay_partial);

        if (rc)
            goto out;
    }

out:
    if (overlay_dtb)
        free(overlay_dtb);

    return rc;
}
/*
 * Local variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
