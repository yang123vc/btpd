#include <sys/types.h>
#include <sys/stat.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "btpd_if.h"
#include "metainfo.h"
#include "subr.h"

const char *btpd_dir;
struct ipc *ipc;

void
btpd_connect(void)
{
    if ((errno = ipc_open(btpd_dir, &ipc)) != 0)
        err(1, "cannot open connection to btpd in %s", btpd_dir);
}

enum ipc_code
handle_ipc_res(enum ipc_code code, const char *target)
{
    switch (code) {
    case IPC_OK:
        break;
    case IPC_FAIL:
        warnx("btpd couldn't execute the requested operation for %s", target);
        break;
    case IPC_ERROR:
        warnx("btpd encountered an error for %s", target);
        break;
    default:
        errx(1, "fatal error in communication with btpd");
    }
    return code;
}

void
print_state_name(struct tpstat *ts)
{
    char statec[] = ">*<U";
    int state = min(ts->state, 3);
    printf("%c. %s", statec[state], ts->name);
}

void
print_stat(struct tpstat *cur)
{
    printf("%5.1f%% %6.1fM %7.2fkB/s %6.1fM %7.2fkB/s %4u %5.1f%%",
        100.0 * cur->have / cur->total,
        (double)cur->downloaded / (1 << 20),
        (double)cur->rate_down / (20 << 10),
        (double)cur->uploaded / (1 << 20),
        (double)cur->rate_up / (20 << 10),
        cur->npeers,
        100.0 * cur->nseen / cur->npieces);
    if (cur->errors > 0)
        printf(" E%u", cur->errors);
    printf("\n");
}

void
usage_add(void)
{
    printf(
        "Add torrents to btpd.\n"
        "\n"
        "Usage: add [--topdir] -d dir file\n"
        "       add file ...\n"
        "\n"
        "Arguments:\n"
        "file ...\n"
        "\tOne or more torrents to add.\n"
        "\n"
        "Options:\n"
        "-d dir\n"
        "\tUse the dir for content.\n"
        "\n"
        "--topdir\n"
        "\tAppend the torrent top directory (if any) to the content path.\n"
        "\tThis option cannot be used without the '-d' option.\n"
        "\n"
        );
    exit(1);
}

struct option add_opts [] = {
    { "help", no_argument, NULL, 'H' },
    { "topdir", no_argument, NULL, 'T'},
    {NULL, 0, NULL, 0}
};

int
content_link(uint8_t *hash, char *buf)
{
    int n;
    char relpath[41];
    char path[PATH_MAX];
    for (int i = 0; i < 20; i++)
        snprintf(relpath + i * 2, 3, "%.2x", hash[i]);
    snprintf(path, PATH_MAX, "%s/torrents/%s/content", btpd_dir, relpath);
    if ((n = readlink(path, buf, PATH_MAX)) == -1)
        return errno;
    buf[min(n, PATH_MAX)] = '\0';
    return 0;
}

void
cmd_add(int argc, char **argv)
{
    int ch, topdir = 0;
    char *dir = NULL, bdir[PATH_MAX];

    while ((ch = getopt_long(argc, argv, "d:", add_opts, NULL)) != -1) {
        switch (ch) {
        case 'T':
            topdir = 1;
            break;
        case 'd':
            dir = optarg;
            break;
        default:
            usage_add();
        }
    }
    argc -= optind;
    argv += optind;

    if (argc < 1 || (topdir == 1 && dir == NULL) || (dir != NULL && argc > 1))
        usage_add();

    if (dir != NULL)
        if (realpath(dir, bdir) == NULL)
            err(1, "path error on %s", bdir);

    btpd_connect();
    for (int i = 0; i < argc; i++) {
        struct metainfo *mi;
        char dpath[PATH_MAX], fpath[PATH_MAX];

        if ((errno = load_metainfo(argv[i], -1, 0, &mi)) != 0) {
            warn("error loading torrent %s", argv[i]);
            continue;
        }

        if ((topdir &&
                !(mi->nfiles == 1
                    && strcmp(mi->name, mi->files[0].path) == 0)))
            snprintf(dpath, PATH_MAX, "%s/%s", bdir, mi->name);
        else if (dir != NULL)
            strlcpy(dpath, bdir, PATH_MAX);
        else {
            if (content_link(mi->info_hash, dpath) != 0) {
                warnx("unknown content dir for %s", argv[i]);
                errx(1, "use the '-d' option");
            }
        }

        if (mkdir(dpath, 0777) != 0 && errno != EEXIST)
            err(1, "couldn't create directory %s", dpath);

        if (realpath(argv[i], fpath) == NULL)
            err(1, "path error on %s", fpath);

        handle_ipc_res(btpd_add(ipc, mi->info_hash, fpath, dpath), argv[i]);
        clear_metainfo(mi);
        free(mi);
    }
}

void
usage_del(void)
{
    printf(
        "Remove torrents from btpd.\n"
        "\n"
        "Usage: del file ...\n"
        "\n"
        "Arguments:\n"
        "file ...\n"
        "\tThe torrents to remove.\n"
        "\n");
    exit(1);
}

void
cmd_del(int argc, char **argv)
{
    if (argc < 2)
        usage_del();

    btpd_connect();
    for (int i = 1; i < argc; i++) {
        struct metainfo *mi;
        if ((errno = load_metainfo(argv[i], -1, 0, &mi)) != 0) {
            warn("error loading torrent %s", argv[i]);
            continue;
        }
        handle_ipc_res(btpd_del(ipc, mi->info_hash), argv[i]);
        clear_metainfo(mi);
        free(mi);
    }
}

void
usage_kill(void)
{
    printf(
        "Shutdown btpd.\n"
        "\n"
        "Usage: kill [seconds]\n"
        "\n"
        "Arguments:\n"
        "seconds\n"
        "\tThe number of seconds btpd waits before giving up on unresponsive\n"
        "\ttrackers.\n"
        "\n"
        );
    exit(1);
}

void
cmd_kill(int argc, char **argv)
{
    int seconds = -1;
    char *endptr;

    if (argc == 2) {
        seconds = strtol(argv[1], &endptr, 10);
        if (strlen(argv[1]) > endptr - argv[1] || seconds < 0)
            usage_kill();
    } else if (argc > 2)
        usage_kill();

    btpd_connect();
    handle_ipc_res(btpd_die(ipc, seconds), "kill");
}

void
usage_list(void)
{
    printf(
        "List active torrents.\n"
        "\n"
        "Usage: list\n"
        "\n"
        );
    exit(1);
}

void
cmd_list(int argc, char **argv)
{
    struct btstat *st;

    if (argc > 1)
        usage_list();

    btpd_connect();
    if (handle_ipc_res(btpd_stat(ipc, &st), "list") != IPC_OK)
        exit(1);
    for (int i = 0; i < st->ntorrents; i++) {
        print_state_name(&st->torrents[i]);
        putchar('\n');
    }
    printf("%u torrent%s.\n", st->ntorrents,
        st->ntorrents == 1 ? "" : "s");
}

void
usage_stat(void)
{
    printf(
        "Display stats for active torrents.\n"
        "The displayed stats are:\n"
        "%% got, MB down, rate down. MB up, rate up\n"
        "peer count, %% of pieces seen, tracker errors\n"
        "\n"
        "Usage: stat [-i] [-w seconds]\n"
        "\n"
        "Options:\n"
        "-i\n"
        "\tDisplay indivudal lines for each active torrent.\n"
        "\n"
        "-w n\n"
        "\tDisplay stats every n seconds.\n"
        "\n");
    exit(1);
}

void
do_stat(int individual, int seconds)
{
    struct btstat *st;
    struct tpstat tot;
again:
    bzero(&tot, sizeof(tot));
    tot.state = T_ACTIVE;
    if (handle_ipc_res(btpd_stat(ipc, &st), "stat") != IPC_OK)
        exit(1);
    for (int i = 0; i < st->ntorrents; i++) {
        struct tpstat *cur = &st->torrents[i];
        tot.uploaded += cur->uploaded;
        tot.downloaded += cur->downloaded;
        tot.rate_up += cur->rate_up;
        tot.rate_down += cur->rate_down;
        tot.npeers += cur->npeers;
        tot.nseen += cur->nseen;
        tot.npieces += cur->npieces;
        tot.have += cur->have;
        tot.total += cur->total;
        if (individual) {
            print_state_name(cur);
            printf(":\n");
            print_stat(cur);
        }
    }
    free_btstat(st);
    if (individual)
        printf("Total:\n");
    print_stat(&tot);
    if (seconds > 0) {
        sleep(seconds);
        goto again;
    }
}

struct option stat_opts [] = {
    { "help", no_argument, NULL, 'H' },
    {NULL, 0, NULL, 0}
};

void
cmd_stat(int argc, char **argv)
{
    int ch;
    int wflag = 0, iflag = 0, seconds = 0;
    char *endptr;
    while ((ch = getopt_long(argc, argv, "iw:", stat_opts, NULL)) != -1) {
        switch (ch) {
        case 'i':
            iflag = 1;
            break;
        case 'w':
            wflag = 1;
            seconds = strtol(optarg, &endptr, 10);
            if (strlen(optarg) > endptr - optarg || seconds < 1)
                usage_stat();
            break;
        default:
            usage_stat();
        }
    }
    argc -= optind;
    argv += optind;
    if (argc > 0)
        usage_stat();

    btpd_connect();
    do_stat(iflag, seconds);
}

struct {
    const char *name;
    void (*fun)(int, char **);
    void (*help)(void);
} cmd_table[] = {
    { "add", cmd_add, usage_add },
    { "del", cmd_del, usage_del },
    { "kill", cmd_kill, usage_kill },
    { "list", cmd_list, usage_list },
    { "stat", cmd_stat, usage_stat }
};

int ncmds = sizeof(cmd_table) / sizeof(cmd_table[0]);

void
usage(void)
{
    printf(
        "btcli is the btpd command line interface.\n"
        "\n"
        "Usage: btcli [main options] command [command options]\n"
        "\n"
        "Main options:\n"
        "-d dir\n"
        "\tThe btpd directory.\n"
        "\n"
        "--help [command]\n"
        "\tShow this text or help for the specified command.\n"
        "\n"
        "Commands:\n"
        "add\n"
        "del\n"
        "kill\n"
        "list\n"
        "stat\n"
        "\n");
    exit(1);
}

struct option base_opts [] = {
    { "help", no_argument, NULL, 'H' },
    {NULL, 0, NULL, 0}
};

int
main(int argc, char **argv)
{
    int ch, help = 0;

    if (argc < 2)
        usage();

    while ((ch = getopt_long(argc, argv, "+d:", base_opts, NULL)) != -1) {
        switch (ch) {
        case 'd':
            btpd_dir = optarg;
            break;
        case 'H':
            help = 1;
            break;
        default:
            usage();
        }
    }
    argc -= optind;
    argv += optind;

    if (argc == 0)
        usage();

    if (btpd_dir == NULL)
        if ((btpd_dir = find_btpd_dir()) == NULL)
            errx(1, "cannot find the btpd directory");

    optind = 0;
    int found = 0;
    for (int i = 0; !found && i < ncmds; i++) {
        if (strcmp(argv[0], cmd_table[i].name) == 0) {
            found = 1;
            if (help)
                cmd_table[i].help();
            else
                cmd_table[i].fun(argc, argv);
        }
    }

    if (!found)
        usage();

    return 0;
}
