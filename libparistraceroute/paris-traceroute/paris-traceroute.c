#include <stdlib.h>                  // malloc...
#include <stdio.h>                   // perror, printf
#include <stdint.h>                  // UINT16_MAX
#include <stdbool.h>                 // bool
#include <errno.h>                   // EINVAL, ENOMEM, errno
#include <libgen.h>                  // basename
#include <string.h>                  // strcmp
#include <sys/types.h>               // gai_strerror
#include <sys/socket.h>              // gai_strerror
#include <netdb.h>                   // gai_strerror

#include "optparse.h"                // opt_*()
#include "pt_loop.h"                 // pt_loop_t
#include "probe.h"                   // probe_t
#include "lattice.h"                 // lattice_t
#include "algorithm.h"               // algorithm_instance_t
#include "algorithms/mda.h"          // mda_*_t
#include "algorithms/traceroute.h"   // traceroute_options_t
#include "address.h"                 // address_to_string, address_set_host
#include "options.h"

//---------------------------------------------------------------------------
// Command line stuff
//---------------------------------------------------------------------------

#define HELP_a "Traceroute algorithm: one of  'paris-traceroute' [default],'mda'"
#define HELP_4 "Use IPv4"
#define HELP_6 "Use IPv6"
#define HELP_P "Use raw packet of protocol prot for tracerouting: one of 'udp' [default]"
#define HELP_U "Use UDP to particular port for tracerouting (instead of increasing the port per each probe),default port is 53"
#define HELP_n "Do not resolve IP addresses to their domain names"
#define HELP_d "set PORT as destination port (default: 3000)"
#define HELP_s "set PORT as source port (default: 3083)"

const char * algorithm_names[] = {
    "paris-traceroute", // default value
    "mda",
    NULL
};

static bool is_ipv4   = true;
static bool is_ipv6   = false;
static bool is_udp    = false;
static bool do_resolv = true;

const char * protocol_names[] = {
    "udp",
    NULL
};

// Bounded integer parameters | def    min  max         option_enabled
static int  dst_port[4]  = {3000,  0,   UINT16_MAX, 0};
static int  src_port[4]  = {3838,  0,   UINT16_MAX, 1};

struct opt_spec runnable_options[] = {
    // action              sf   lf                   metavar             help         data
    {opt_store_choice,     "a", "--algo",            "ALGORITHM",        HELP_a,      algorithm_names},
    {opt_store_1,          "4", OPT_NO_LF,           OPT_NO_METAVAR,     HELP_4,      &is_ipv4},
    {opt_store_0,          "6", OPT_NO_LF,           OPT_NO_METAVAR,     HELP_6,      &is_ipv6},
    {opt_store_choice,     "P", "--protocol",        "protocol",         HELP_P,      protocol_names},
    {opt_store_1,          "U", "--udp",             OPT_NO_METAVAR,     HELP_U,      &is_udp},
    {opt_store_0,          "n", OPT_NO_LF,           OPT_NO_METAVAR,     HELP_n,      &do_resolv},
    {opt_store_int_lim_en, "s", "--src-port",        "PORT",             HELP_s,      src_port},
    {opt_store_int_lim_en, "d", "--drc-port",        "PORT",             HELP_d,      dst_port},
};

//---------------------------------------------------------------------------
// Main program
//---------------------------------------------------------------------------

// DUPLICATED from traceroute/traceroute.c
// TO MOVE in algorithms/traceroute.*

/**
 * \brief Handle raised traceroute_event_t events.
 * \param loop The main loop.
 * \param traceroute_event The handled event.
 * \param traceroute_options Options related to this instance of traceroute .
 * \param traceroute_data Data related to this instance of traceroute.
 */

void my_traceroute_handler(
    pt_loop_t                  * loop,
    traceroute_event_t         * traceroute_event,
    const traceroute_options_t * traceroute_options,
    const traceroute_data_t    * traceroute_data
) {
    const probe_t * probe;
    const probe_t * reply;
    char          * discovered_hostname = NULL;
    static size_t   num_probes_printed = 0;

    switch (traceroute_event->type) {
        case TRACEROUTE_PROBE_REPLY:
            // Retrieve the probe and its corresponding reply
            probe = ((const probe_reply_t *) traceroute_event->data)->probe;
            reply = ((const probe_reply_t *) traceroute_event->data)->reply;

            // Print TTL (if this is the first probe related to this TTL)
            if (num_probes_printed % traceroute_options->num_probes == 0) {
                uint8_t ttl;
                if (probe_extract(probe, "ttl", &ttl)) {
                    printf("%-2d", ttl);
                }
            }

            // Print discovered IP
            {
                char * discovered_ip;
                if (probe_extract(reply, "src_ip", &discovered_ip)) {
                    printf(" %-16s ", discovered_ip);
                    if (do_resolv) {
                        address_resolv(discovered_ip, &discovered_hostname);
                        if (discovered_hostname) printf("(%s)", discovered_hostname);
                    }
                    free(discovered_ip);
                }
            }

            // Print delay
            {
                double send_time = probe_get_sending_time(probe),
                       recv_time = probe_get_recv_time(reply);
                printf(" (%-5.2lfms) ", 1000 * (recv_time - send_time));
            }
            num_probes_printed++;
            break;
        case TRACEROUTE_STAR:
            printf(" *");
            num_probes_printed++;
            break;
        case TRACEROUTE_ICMP_ERROR:
            printf(" !");
            num_probes_printed++;
            break;
        case TRACEROUTE_TOO_MANY_STARS:
            printf("Too many stars\n");
            break;
        case TRACEROUTE_MAX_TTL_REACHED:
            printf("Max ttl reached\n");
            break;
        case TRACEROUTE_DESTINATION_REACHED:
            // The traceroute algorithm has terminated.
            // We could print additional results.
            // Interrupt the main loop.
            printf("Destination reached\n");
            break;
        default:
            break;
    }

    if (num_probes_printed % traceroute_options->num_probes == 0) {
        printf("\n");
    }
}

/**
 * \brief Handle events raised by libparistraceroute
 * \param loop The main loop
 * \param event The event raised by libparistraceroute
 * \param user_data Points to user data, shared by
 *   all the algorithms instances running in this loop.
 */

void algorithm_handler(pt_loop_t * loop, event_t * event, void * user_data)
{
    traceroute_event_t         * traceroute_event;
    const traceroute_options_t * traceroute_options;
    const traceroute_data_t    * traceroute_data;
    mda_event_t                * mda_event;
    const char                 * algorithm_name;

    switch (event->type) {
        case ALGORITHM_TERMINATED:
            algorithm_name = event->issuer->algorithm->name;
            if (strcmp(algorithm_name, "mda") == 0) {
                // Dump full lattice, only when MDA_NEW_LINK is not handled
                mda_interface_dump(event->issuer->data, do_resolv);
            }
            pt_loop_terminate(loop);
            break;
        case ALGORITHM_EVENT:
            algorithm_name = event->issuer->algorithm->name;
            if (strcmp(algorithm_name, "mda") == 0) {
                mda_event = event->data;
                switch (mda_event->type) {
                    case MDA_NEW_LINK:
                        mda_link_dump(mda_event->data, do_resolv);
                        break;
                    default:
                        break;
                }
            } else if (strcmp(algorithm_name, "traceroute") == 0) {
                traceroute_event   = event->data;
                traceroute_options = event->issuer->options;
                traceroute_data    = event->issuer->data;
                my_traceroute_handler(loop, traceroute_event, traceroute_options, traceroute_data);
            }
            break;
        default:
            break;
    }
}

/**
 * \brief Prepare options supported by paris-traceroute
 * \return A pointer to the corresponding options_t instance if successfull, NULL otherwise
 */

static options_t * init_options(const char * version) {
    options_t * options;

    // Building the command line options
    // TODO add marker to end options (like protocol_field)
    if (!(options = options_create(NULL))) {
        goto ERR_OPTIONS_CREATE;
    }

    // We have to add traceroute command line options before those of mda
    // to manage properly colliding options 
    options_add_options(options, traceroute_get_cl_options(), 2);
    options_add_options(options, mda_get_cl_options(),        3);
    options_add_options(options, network_get_cl_options(),    1);
    options_add_options(options, runnable_options,            9);
    options_add_common (options, version);
    return options;

ERR_OPTIONS_CREATE:
    return NULL;
}

int main(int argc, char ** argv)
{
    traceroute_options_t      traceroute_options;
    traceroute_options_t    * ptraceroute_options;
    mda_options_t             mda_options;
    probe_t                 * probe;
    pt_loop_t               * loop;
    int                       exit_code = EXIT_FAILURE;
    int                       family;
    address_t                 dst_addr;
    options_t               * options;
    const char              * version = "version 1.0";
    char                    * dst_ip;
    const char              * algorithm_name = algorithm_names[0]; 
    const char              * ip_protocol_name;
    void                    * algorithm_options;

    if (!(options = init_options(version))) {
        fprintf(stderr, "E: Can't initialize options\n");
        goto ERR_INIT_OPTIONS;
    }

    // Retrieve values passed in the command-line
    opt_options1st();
    if (opt_parse("usage: %s [options] host", (struct opt_spec *)(options->optspecs->cells), argv) != 1) {
        fprintf(stderr, "%s: destination required\n", basename(argv[0]));
        goto ERR_OPT_PARSE;
    }

    // We assume that the target IP address is always the last argument
    dst_ip = argv[argc - 1];
    printf("dst_ip = %s\n", dst_ip);

    // Verify that the user pass option related to mda iif this is the chosen algorithm.
    if (options_mda_get_is_set()) {
        if (strcmp(algorithm_name, "mda") != 0) {
            perror("E: You cannot pass options related to mda when using another algorithm ");
            goto ERR_INVALID_ALGORITHM;
        }
    }

    // TODO akram: test whether -4 and/or -6 have been set.
    // If no one is set, call address_guess_family.
    // If only one is set to true, set family to AF_INET or AF_INET6
    // If the both have been set, print an error.
    /*
    if (!is_ipv4 && !is_ipv6) {
        fprintf(stderr, "E: Cannot guess address family of this address %s\n", dst_ip);
        goto ERR_ADDRESS_GUESS_FAMILY;
    }
    */
    // Get address family if not defined by the user
    address_guess_family(dst_ip, &family);

    switch (family) {
        case AF_INET:
            ip_protocol_name = "ipv4";
            break;
        case AF_INET6:
            ip_protocol_name = "ipv6";
            break;
        default:
            fprintf(stderr, "Internet family not supported (%d)\n", family);
            goto ERR_FAMILY;
    }

    // Translate the string IP / FQDN into an address_t * instance 
    if (address_from_string(family, dst_ip, &dst_addr) != 0) {
        fprintf(stderr, "E: Invalid destination address %s\n", dst_ip);
        goto ERR_ADDRESS_IP_FROM_STRING;
    }

    //// DEBUG
    printf("Address\n");
    address_dump(&dst_addr);
    printf("\n");

    // If dst_ip is not a FQDN, we've to get the corresponding IP string.
    // This is a crappy patch while ipv*.c protocols module do not use field carrying address_t instance
    if ((address_to_string(&dst_addr, &dst_ip)) != 0) goto ERR_ADDRESS_TO_STRING;

    // Prepare data
    printf("Traceroute to %s using algorithm %s\n\n", dst_ip, algorithm_name);

    // Probe skeleton definition: IPv4/UDP probe targetting 'dst_ip'
    if (!(probe = probe_create())) {
        perror("E: Cannot create probe skeleton");
        goto ERR_PROBE_SKEL;
    }

    probe_set_protocols(
        probe,
        ip_protocol_name,
        is_udp  ? "udp"  : protocol_names[0],
        NULL
    );
    probe_payload_resize(probe, 2);

    // Set default values
    probe_set_fields(
        probe,
        STR("dst_ip",   dst_ip),
        I16("dst_port", dst_port[0]),
        I16("src_port", src_port[0]),
        NULL
    );

    // Option -U sets port to 53 (DNS)
    if (is_udp && !dst_port[3]) {
        probe_set_fields(probe, I16("dst_port", 53), NULL);
    }
    probe_dump(probe);
    // Dedicated options (if any)
    if (strcmp(algorithm_name, "paris-traceroute") == 0) {
        traceroute_options  = traceroute_get_default_options();
        ptraceroute_options = &traceroute_options;
        algorithm_options   = &traceroute_options;
        algorithm_name      = "traceroute";
    } else if ((strcmp(algorithm_name, "mda") == 0) || options_mda_get_is_set()) {
        mda_options            = mda_get_default_options();
        ptraceroute_options    = &mda_options.traceroute_options;
        algorithm_options      = &mda_options;
        mda_options.bound      = options_mda_get_bound();
        mda_options.max_branch = options_mda_get_max_branch();
    } else {
        perror("E: Unknown algorithm ");
        goto ERR_UNKNOWN_ALGORITHM;
    }

    // Common options
    if (ptraceroute_options) {
        ptraceroute_options->min_ttl = options_traceroute_get_min_ttl();
        ptraceroute_options->max_ttl = options_traceroute_get_max_ttl();
        ptraceroute_options->dst_ip  = dst_ip;
    }

    // Create libparistraceroute loop
    if (!(loop = pt_loop_create(algorithm_handler, NULL))) {
        perror("E: Cannot create libparistraceroute loop");
        goto ERR_LOOP_CREATE;
    }

    // Set network timeout
    network_set_timeout(loop->network, options_network_get_timeout());

    // Add an algorithm instance in the main loop
    if (!pt_algorithm_add(loop, algorithm_name, algorithm_options, probe)) {
        perror("E: Cannot add the chosen algorithm");
        goto ERR_INSTANCE;
    }
    printf("algo added\n");
    // Wait for events. They will be catched by handler_user()
    if (pt_loop(loop, 0) < 0) {
        perror("E: Main loop interrupted");
        goto ERR_PT_LOOP;
    }
    exit_code = EXIT_SUCCESS;

    // Leave the program
ERR_PT_LOOP:
ERR_INSTANCE:
ERR_UNKNOWN_ALGORITHM:
    probe_free(probe);
ERR_PROBE_SKEL:
    // pt_loop_free() automatically removes algorithms instances,
    // probe_replies and events from the memory.
    // Options and probe must be manually removed.
    pt_loop_free(loop);
ERR_LOOP_CREATE:
ERR_ADDRESS_TO_STRING:
ERR_ADDRESS_IP_FROM_STRING:
ERR_FAMILY:
ERR_ADDRESS_GUESS_FAMILY:
ERR_INVALID_ALGORITHM:
    if (errno) perror(gai_strerror(errno));
ERR_OPT_PARSE:
ERR_INIT_OPTIONS:
    exit(exit_code);
}

