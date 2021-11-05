#include <stdbool.h>
#include <syslog.h>
#include <stdio.h>
#include <err.h>
#include <stdlib.h>
#include <xenstore.h>
#include <xenctrl.h>
#include <xenguest.h>
#include <libxl.h>
#include <xenevtchn.h>

#include "init-dom-json.h"

#define NR_MAGIC_PAGES 4
#define CONSOLE_PFN_OFFSET 0
#define XENSTORE_PFN_OFFSET 1
#define STR_MAX_LENGTH 64

static int alloc_magic_pages(struct xc_dom_image *dom)
{
    int rc, i;
    const xen_pfn_t base = GUEST_MAGIC_BASE >> XC_PAGE_SHIFT;
    xen_pfn_t p2m[NR_MAGIC_PAGES];

    for (i = 0; i < NR_MAGIC_PAGES; i++)
        p2m[i] = base + i;

    rc = xc_domain_populate_physmap_exact(dom->xch, dom->guest_domid,
                                          NR_MAGIC_PAGES, 0, 0, p2m);
    if (rc < 0)
        return rc;

    dom->xenstore_pfn = base + XENSTORE_PFN_OFFSET;

    xc_clear_domain_page(dom->xch, dom->guest_domid, dom->xenstore_pfn);

    xc_hvm_param_set(dom->xch, dom->guest_domid, HVM_PARAM_STORE_PFN,
                     dom->xenstore_pfn);
    xc_hvm_param_set(dom->xch, dom->guest_domid, HVM_PARAM_STORE_EVTCHN,
                     dom->xenstore_evtchn);
    return 0;
}

static void do_xs_write(struct xs_handle *xsh, xs_transaction_t t,
                        char *path, char *val)
{
    if (!xs_write(xsh, t, path, val, strlen(val)))
        fprintf(stderr, "writing %s to xenstore failed.\n", path);
}

static void do_xs_write_dom(struct xs_handle *xsh, xs_transaction_t t,
                            domid_t domid, char *path, char *val)
{
    char full_path[STR_MAX_LENGTH];

    snprintf(full_path, STR_MAX_LENGTH,
             "/local/domain/%d/%s", domid, path);
    do_xs_write(xsh, t, full_path, val);
}

static void do_xs_write_libxl(struct xs_handle *xsh, xs_transaction_t t,
                              domid_t domid, char *path, char *val)
{
    char full_path[STR_MAX_LENGTH];

    snprintf(full_path, STR_MAX_LENGTH,
             "/libxl/%d/%s", domid, path);
    do_xs_write(xsh, t, full_path, val);
}

static void do_xs_write_vm(struct xs_handle *xsh, xs_transaction_t t,
                           libxl_uuid uuid, char *path, char *val)
{
    char full_path[STR_MAX_LENGTH];

    snprintf(full_path, STR_MAX_LENGTH,
             "/vm/" LIBXL_UUID_FMT "/%s", LIBXL_UUID_BYTES(uuid), path);
    do_xs_write(xsh, t, full_path, val);
}

static int restore_xenstore(struct xs_handle *xsh,
                            libxl_dominfo *info, libxl_uuid uuid,
                            evtchn_port_t xenstore_port)
{
    domid_t domid;
    int i;
    char uuid_str[STR_MAX_LENGTH];
    char dom_name_str[STR_MAX_LENGTH];
    char vm_val_str[STR_MAX_LENGTH];
    char id_str[STR_MAX_LENGTH];
    char max_memkb_str[STR_MAX_LENGTH];
    char cpu_str[STR_MAX_LENGTH];
    char xenstore_port_str[STR_MAX_LENGTH];
    char ring_ref_str[STR_MAX_LENGTH];
    xs_transaction_t t;

    domid = info->domid;
    snprintf(id_str, STR_MAX_LENGTH, "%d", domid);
    snprintf(dom_name_str, STR_MAX_LENGTH, "dom0less-%d", domid);
    snprintf(uuid_str, STR_MAX_LENGTH, LIBXL_UUID_FMT, LIBXL_UUID_BYTES(uuid));
    snprintf(vm_val_str, STR_MAX_LENGTH,
             "vm/" LIBXL_UUID_FMT, LIBXL_UUID_BYTES(uuid));
    snprintf(max_memkb_str, STR_MAX_LENGTH, "%lu", info->max_memkb);
    snprintf(ring_ref_str, STR_MAX_LENGTH, "%lld",
             (GUEST_MAGIC_BASE >> XC_PAGE_SHIFT) + XENSTORE_PFN_OFFSET);
    snprintf(xenstore_port_str, STR_MAX_LENGTH, "%d", xenstore_port);

retry_transaction:
    t = xs_transaction_start(xsh);
    if (t == XBT_NULL)
        return errno;

    /* /vm */
    do_xs_write_vm(xsh, t, uuid, "name", dom_name_str);
    do_xs_write_vm(xsh, t, uuid, "uuid", uuid_str);
    do_xs_write_vm(xsh, t, uuid, "start_time", "0");

    /* /domain */
    do_xs_write_dom(xsh, t, domid, "vm", vm_val_str);
    do_xs_write_dom(xsh, t, domid, "name", dom_name_str);
    do_xs_write_dom(xsh, t, domid, "cpu", "");
    for (i = 0; i < info->vcpu_max_id; i++) {
        snprintf(cpu_str, STR_MAX_LENGTH, "cpu/%d/availability/", i);
        do_xs_write_dom(xsh, t, domid, cpu_str,
                        (info->cpupool & (1 << i)) ? "online" : "offline");
    }
    do_xs_write_dom(xsh, t, domid, "cpu/0", "");
    do_xs_write_dom(xsh, t, domid, "cpu/availability", "online");

    do_xs_write_dom(xsh, t, domid, "memory", "");
    do_xs_write_dom(xsh, t, domid, "memory/static-max", max_memkb_str);
    do_xs_write_dom(xsh, t, domid, "memory/videoram", "-1");

    do_xs_write_dom(xsh, t, domid, "device", "");
    do_xs_write_dom(xsh, t, domid, "device/suspend", "");
    do_xs_write_dom(xsh, t, domid, "device/suspend/event-channel", "");

    do_xs_write_dom(xsh, t, domid, "control", "");
    do_xs_write_dom(xsh, t, domid, "control/shutdown", "");
    do_xs_write_dom(xsh, t, domid, "control/feature-poweroff", "1");
    do_xs_write_dom(xsh, t, domid, "control/feature-reboot", "1");
    do_xs_write_dom(xsh, t, domid, "control/feature-suspend", "");
    do_xs_write_dom(xsh, t, domid, "control/sysrq", "");
    do_xs_write_dom(xsh, t, domid, "control/platform-feature-multiprocessor-suspend", "1");
    do_xs_write_dom(xsh, t, domid, "control", "platform-feature-xs_reset_watches");

    do_xs_write_dom(xsh, t, domid, "domid", id_str);
    do_xs_write_dom(xsh, t, domid, "data", "");
    do_xs_write_dom(xsh, t, domid, "drivers", "");
    do_xs_write_dom(xsh, t, domid, "feature", "");
    do_xs_write_dom(xsh, t, domid, "attr", "");

    do_xs_write_dom(xsh, t, domid, "store/port", xenstore_port_str);
    do_xs_write_dom(xsh, t, domid, "store/ring-ref", ring_ref_str);

    do_xs_write_libxl(xsh, t, domid, "type", "pvh");
    do_xs_write_libxl(xsh, t, domid, "dm-version", "qemu_xen");

    if (!xs_transaction_end(xsh, t, false))
        if (errno == EAGAIN)
            goto retry_transaction;

    return 0;
}

static int init_domain(struct xs_handle *xsh, libxl_dominfo *info)
{
    struct xc_dom_image dom;
    libxl_uuid uuid;
    uint64_t v;
    int rc;

    printf("#### Init dom0less domain: %d ####\n", info->domid);
    dom.guest_domid = info->domid;
    dom.xenstore_domid = 0;
    dom.xch = xc_interface_open(0, 0, 0);

    rc = xc_hvm_param_get(dom.xch, info->domid, HVM_PARAM_STORE_EVTCHN, &v);
    if (rc != 0) {
        printf("Failed to get HVM_PARAM_STORE_EVTCHN\n");
        return 1;
    }
    dom.xenstore_evtchn = v;

    /* Console won't be initialized but set its data for completeness */
    dom.console_domid = 0;

    /* Alloc magic pages */
    printf("Allocating magic pages\n");
    if (alloc_magic_pages(&dom) != 0) {
        printf("Error on alloc magic pages\n");
        return 1;
    }

    printf("Setup Grant Tables\n");
    xc_dom_gnttab_init(&dom);

    printf("Setup UUID\n");
    libxl_uuid_generate(&uuid);
    xc_domain_sethandle(dom.xch, info->domid, libxl_uuid_bytearray(&uuid));

    printf("Creating JSON\n");
    rc = gen_stub_json_config(info->domid, &uuid);
    if (rc)
        err(1, "gen_stub_json_config");

    printf("Restoring Xenstore values\n");
    restore_xenstore(xsh, info, uuid, dom.xenstore_evtchn);

    printf("Introducing domain\n");
    xs_introduce_domain(xsh, info->domid,
            (GUEST_MAGIC_BASE >> XC_PAGE_SHIFT) + XENSTORE_PFN_OFFSET,
            dom.xenstore_evtchn, true);
    return 0;
}

/* Check if domain has been configured in XS */
static bool domain_exists(struct xs_handle *xsh, int domid)
{
    return xs_is_domain_introduced(xsh, domid);
}

int main(int argc, char **argv)
{
    libxl_dominfo *info;
    libxl_ctx *ctx;
    int nb_vm, i;
    struct xs_handle *xsh;

    xsh = xs_daemon_open();
    if (xsh == NULL) {
        fprintf(stderr, "Could not contact XenStore");
        exit(1);
    }

    if (libxl_ctx_alloc(&ctx, LIBXL_VERSION, 0, NULL)) {
        fprintf(stderr, "cannot init xl context\n");
        exit(1);
    }

    info = libxl_list_domain(ctx, &nb_vm);
    if (!info) {
        fprintf(stderr, "libxl_list_vm failed.\n");
        exit(EXIT_FAILURE);
    }

    for (i = 0; i < nb_vm; i++) {
        domid_t domid = info[i].domid;

        /* Don't need to check for Dom0 */
        if (!domid)
            continue;

        printf("Checking domid: %u\n", domid);
        if (!domain_exists(xsh, domid))
            init_domain(xsh, &info[i]);
        else
            printf("Domain %d has already been initialized\n", domid);
    }
    libxl_dominfo_list_free(info, nb_vm);
    xs_close(xsh);
    return 0;
}
