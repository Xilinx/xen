#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <xenstore.h>
#include <libxenvchan.h>
#include <xengnttab.h>

#define PAGE_SIZE 4096

static int xs_create_overlay_node(int domain, const char *xs_base,
                                  struct xs_handle *xs)
{
    int ret = -1;
    struct xs_permissions perms[2];
    char buf[64];
    char ref[16];
    char* domid_str = NULL;
    int overlay_size = 0;

    xs_transaction_t xs_trans = XBT_NULL;

    domid_str = xs_read(xs, XBT_NULL, "domid", NULL);

    if (!domid_str)
        return ret;

    /* owner domain is us */
    perms[0].id = atoi(domid_str);
    /* permissions for domains not listed = none. */
    perms[0].perms = XS_PERM_NONE;
    /* other domains i.e. domain provided by user gets r/w permissions. */
    perms[1].id = domain;
    perms[1].perms =  XS_PERM_READ | XS_PERM_WRITE;

retry_transaction:

    xs_trans = xs_transaction_start(xs);
    if (!xs_trans)
        goto fail_xs_transaction;

    /* Create overlay-size node. */
    snprintf(ref, sizeof(ref), "%d", overlay_size);
    snprintf(buf, sizeof(buf), "%s/overlay-size", xs_base);

    if (!xs_write(xs, xs_trans, buf, ref, strlen(ref)))
        goto fail_xs_transaction;
    if (!xs_set_permissions(xs, xs_trans, buf, perms, 2))
        goto fail_xs_transaction;

    /* Create domU status node. */
    snprintf(ref, sizeof(ref), "%s", "waiting");
    snprintf(buf, sizeof(buf), "%s/receiver-status", xs_base);

    if (!xs_write(xs, xs_trans, buf, ref, strlen(ref)))
        goto fail_xs_transaction;
    if (!xs_set_permissions(xs, xs_trans, buf, perms, 2))
        goto fail_xs_transaction;

    /* Create dom0 status node. */
    snprintf(ref, sizeof(ref), "%s", "not_ready");
    snprintf(buf, sizeof(buf), "%s/sender-status", xs_base);

    if (!xs_write(xs, xs_trans, buf, ref, strlen(ref)))
        goto fail_xs_transaction;
    if (!xs_set_permissions(xs, xs_trans, buf, perms, 2))
        goto fail_xs_transaction;

    /* Create overlay-name node. */
    snprintf(ref, sizeof(ref), "%s", "overlay_node");
    snprintf(buf, sizeof(buf), "%s/overlay-name", xs_base);

    if (!xs_write(xs, xs_trans, buf, ref, strlen(ref)))
        goto fail_xs_transaction;
    if (!xs_set_permissions(xs, xs_trans, buf, perms, 2))
        goto fail_xs_transaction;

    /* Create overlay-type node. */
    snprintf(ref, sizeof(ref), "%s", "type");
    snprintf(buf, sizeof(buf), "%s/overlay-type", xs_base);

    if (!xs_write(xs, xs_trans, buf, ref, strlen(ref)))
        goto fail_xs_transaction;
    if (!xs_set_permissions(xs, xs_trans, buf, perms, 2))
        goto fail_xs_transaction;

    /* Create overlay-partial node. */
    snprintf(ref, sizeof(ref), "%d", 0);
    snprintf(buf, sizeof(buf), "%s/overlay-partial", xs_base);

    if (!xs_write(xs, xs_trans, buf, ref, strlen(ref)))
        goto fail_xs_transaction;
    if (!xs_set_permissions(xs, xs_trans, buf, perms, 2))
        goto fail_xs_transaction;

    if (!xs_transaction_end(xs, xs_trans, 0)) {
        if (errno == EAGAIN)
            goto retry_transaction;
        else
            goto fail_xs_transaction;
    } else
        ret = 0;

fail_xs_transaction:
    free(domid_str);

    return ret;
}

static int get_overlay_size(struct xs_handle *xs, int domain,
                            const char *xs_path)
{
    char buf[128];
    char *ref;
    unsigned int len;
    int dt_size = 0;

    snprintf(buf, sizeof(buf), "%s/overlay-size", xs_path);

    ref = xs_read(xs, XBT_NULL, buf, &len);

    if (!ref)
        return dt_size;

    dt_size = atoi(ref);

    free(ref);

    return dt_size;
}

static uint32_t get_num_pages(int dtbo_size)
{
    int num_pages = 1;

    while (dtbo_size > PAGE_SIZE) {
        dtbo_size = dtbo_size - PAGE_SIZE;
        num_pages++;
    }

    return num_pages;
}

static void *create_shared_buffer(int domain, uint32_t *refs, uint32_t pages,
                                  xengntshr_handle *gntshr)
{
    return xengntshr_share_pages(gntshr, domain, pages, refs, 1);
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

static bool write_page_ref(struct xs_handle *xs, uint32_t *page_ref,
                           size_t num_pages, const char *path)
{
    xs_transaction_t xs_trans = XBT_NULL;
    char buf[128];
    char *ref = NULL;
    char tmp[16];
    int i = 0;
    bool rc = false;

    /* Caller will free this. */
    ref = (char *)calloc(num_pages * 16, sizeof(char)); /* For each number. */
    if (ref == NULL) {
        fprintf(stderr, "Memory calloc for ref failed\n");
        return rc;
    }

retry_transaction:
    xs_trans = xs_transaction_start(xs);
    if (!xs_trans)
        goto out;

    for (i = 0; i < num_pages; i++) {
        snprintf(tmp, sizeof(tmp), "%d,", page_ref[i]);
        strcat(ref, tmp);
    }

    snprintf(buf, sizeof(buf), "%s/page-ref", path);

    if (!xs_write(xs, xs_trans, buf, ref, strlen(ref)))
        goto out;

    snprintf(buf, sizeof(buf), "%s/num-pages", path);
    snprintf(tmp, sizeof(tmp), "%lu", num_pages);
    if (!xs_write(xs, xs_trans, buf, tmp, strlen(tmp)))
        goto out;

    if (!xs_transaction_end(xs, xs_trans, 0)) {
        if (errno == EAGAIN)
            goto retry_transaction;
        else
            goto out;
    }

    rc = true;

out:
    if (ref)
        free(ref);

    return rc;
}

static bool write_status(struct xs_handle *xs, const char *status,
                         const char *status_path)
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

static char *get_overlay_ops(struct xs_handle *xs, const char *xs_path)
{
    char buf[128];
    char *ref = NULL;
    unsigned int len;

    snprintf(buf, sizeof(buf), "%s/overlay-operation", xs_path);

    ref = xs_read(xs, XBT_NULL, buf, &len);

    return ref;
}
static char *get_overlay_name(struct xs_handle *xs, const char *xs_path)
{
    char buf[128];
    char *ref = NULL;
    unsigned int len;

    snprintf(buf, sizeof(buf), "%s/overlay-name", xs_path);

    ref = xs_read(xs, XBT_NULL, buf, &len);

    return ref;
}

static char *get_overlay_type(struct xs_handle *xs, const char *xs_path)
{
    char buf[128];
    char *ref = NULL;
    unsigned int len;

    snprintf(buf, sizeof(buf), "%s/overlay-type", xs_path);

    ref = xs_read(xs, XBT_NULL, buf, &len);

    return ref;
}

static bool get_overlay_partial(struct xs_handle *xs, const char *xs_path)
{
    char buf[128];
    char *ref = NULL;
    unsigned int len;

    snprintf(buf, sizeof(buf), "%s/overlay-partial", xs_path);

    ref = xs_read(xs, XBT_NULL, buf, &len);

    if (ref) {
        free(ref);
        return atoi(ref);
    }

    return false;
}

int main(int argc, char **argv)
{
    void *buffer = NULL;
    int domain ;
    uint32_t *page_refs = NULL;
    FILE *fptr = NULL;
    int dtbo_size = 0;
    const char *path = "data/overlay";
    char receiver_status_path[64] = { };
    char sender_status_path[64] = { };
    struct xs_handle *xs = NULL;
    int rc = 0;
    int fd = 0;
    uint32_t num_pages = 0;
    xengntshr_handle *gntshr = NULL;
    char *overlay_ops = NULL;
    char *name = NULL;
    char *type = NULL;
    bool is_partial = false;

    if (argc < 2) {
       fprintf(stderr,"Please enter domain_id.\n");
        return 0;
    }

    domain = atoi(argv[1]);

    xs = xs_open(0);
    if (xs == NULL) {
        fprintf(stderr, "Xenstore open for domain%d failed\n", domain);
        goto out;
    }

    rc = xs_create_overlay_node(domain, path, xs);
    if (rc) {
        fprintf(stderr,"Creating overlay nodes failed\n");
        goto out;
    }

    strcpy(receiver_status_path, path);
    strcat(receiver_status_path, "/receiver-status");

    strcpy(sender_status_path, path);
    strcat(sender_status_path, "/sender-status");

    /*
     * Watch a node for changes (poll on fd to detect).
     * When the node changes, fd will become readable.
     */
    rc = xs_watch(xs, sender_status_path, "overlay_watch");
    if (rc == 0) {
        fprintf(stderr, "Creating watch failed\n");
        goto out;
    }

    /* We are notified of read availability on the watch via the
     * file descriptor.
     */
    fd = xs_fileno(xs);

    /* Wait for ready. */
    if (!wait_for_status(xs, fd, sender_status_path, "ready")) {
        fprintf(stderr, "dom0 not ready.\n");
        goto out;
    }

    dtbo_size = get_overlay_size(xs, domain, path);
    if (dtbo_size == 0) {
        fprintf(stderr,"Overlay data size is zero. Exiting the application\n");
        goto out;
    }

    gntshr = xengntshr_open(NULL, 0);
    if (!gntshr) {
        fprintf(stderr,"Error in opening gntshr\n");
        goto out;
    }

    num_pages = get_num_pages(dtbo_size);

    page_refs =(uint32_t *)malloc(num_pages * sizeof(int));
    if (page_refs == NULL) {
        fprintf(stderr, "Allocating page_ref array failed\n");
        goto out;
    }

    /* Allocate memory for data size and share with domain. */
    buffer = create_shared_buffer(domain, page_refs, num_pages,
                                  gntshr);
    if (buffer == NULL) {
        fprintf(stderr,"Buffer allocation failed\n");
        goto out;
    }

    /* Created the buffer and got page_ref. Share the page_ref with domain. */
    if (!write_page_ref(xs, page_refs, num_pages, path)) {
        fprintf(stderr,"Writing page ref failed\n");
        goto out;
    }

    /* Write the status "page_ref". */
    if (!write_status(xs, "page_ref", receiver_status_path)) {
        fprintf(stderr,"Writing status DONE failed\n");
        goto out;
    }

    /* Wait for done. This means other domain done copying the dtb to buffer. */
    if (!wait_for_status(xs, fd, sender_status_path, "done")) {
        fprintf(stderr, "dom0 status not done\n");
        goto out;
    }

    overlay_ops = get_overlay_ops(xs, path);
    name = get_overlay_name(xs, path);
    type = get_overlay_type(xs, path);
    is_partial = get_overlay_partial(xs, path);

    if (overlay_ops == NULL || name == NULL || type == NULL)
        goto out;

    printf("%s %s %s", overlay_ops, name, type);
    if (is_partial)
        printf(" %d", is_partial);

    printf("\n");

    if (!strcmp(overlay_ops, "add")) {

        if ((fptr = fopen("overlay.dtbo","wb")) == NULL) {
            fprintf(stderr,"Error! opening file");
            goto out;
        }

        printf("Writing to file overlay.dtbo.\n");

        fwrite(buffer, dtbo_size, 1, fptr);

        printf("Done writing to file overlay.dtbo \n");
    }

out:
    if (fptr)
        fclose(fptr);

    if (page_refs)
        free(page_refs);

    if (overlay_ops)
        free(overlay_ops);

    if (name)
        free(name);

    if (type)
        free(type);

    if (xs) {
        close(fd);

        xs_unwatch(xs, path, "overlay_watch");

        xs_close(xs);
    }

    if (buffer)
        xengntshr_unshare(gntshr, buffer, num_pages);

    if (gntshr)
         xengntshr_close(gntshr);

    return 0;
}
