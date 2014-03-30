#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <limits.h>
#include <pthread.h>
#include <numa.h>
#include <numaif.h>
#include <sys/types.h>
#include <jemalloc/jemalloc.h>

#include "numakind.h"
#include "numakind_hbw.h"


struct numanode_bandwidth_t {
    int numanode;
    int bandwidth;
};

struct bandwidth_nodes_t {
    int bandwidth;
    int num_numanodes;
    int *numanodes;
};

static int parse_node_bandwidth(int num_bandwidth, int *bandwidth,
               const char *bandwidth_path);

static int create_bandwidth_nodes(int num_bandwidth, const int *bandwidth,
               int *num_unique, struct bandwidth_nodes_t **bandwidth_nodes);

static int set_closest_numanode(int num_unique,
               const struct bandwidth_nodes_t *bandwidth_nodes,
               int target_bandwidth, int num_cpunode, int *closest_numanode);

static int numanode_bandwidth_compare(const void *a, const void *b);


int numakind_hbw_isavail(void)
{
    int err;
    err = numakind_hbw_nodemask(NULL, 0);
    return (!err);
}

int numakind_hbw_nodemask(unsigned long *nodemask, unsigned long maxnode)
{
    static int init_err = 0;
    static pthread_mutex_t init_mutex = PTHREAD_MUTEX_INITIALIZER;
    static int numcpu = 0;
    static int *closest_numanode = NULL;
    static int is_init = 0;
    int *bandwidth = NULL;
    int high_bandwidth = 0;
    int num_unique = 0;
    int cpu;
    struct bandwidth_nodes_t *bandwidth_nodes = NULL;
    struct bitmask nodemask_bm = {maxnode, nodemask};

    if (!init_err && !is_init) {
        pthread_mutex_lock(&init_mutex);
        if (!init_err && !is_init) {
            numcpu = numa_num_configured_cpus();
            closest_numanode = (int *)je_malloc(sizeof(int) * numcpu);
            bandwidth = (int *)je_malloc(sizeof(int) * NUMA_NUM_NODES);
            if (!(closest_numanode && bandwidth)) {
                init_err = NUMAKIND_ERROR_MALLOC;
            }
            if (!init_err) {
                init_err = parse_node_bandwidth(NUMA_NUM_NODES, bandwidth,
                                                NUMAKIND_BANDWIDTH_PATH);
            }
            if (!init_err) {
                init_err = create_bandwidth_nodes(NUMA_NUM_NODES, bandwidth,
                                                  &num_unique,
                                                  &bandwidth_nodes);
            }
            if (!init_err) {
                if (num_unique == 1) {
                    init_err = NUMAKIND_ERROR_UNAVAILABLE;
                }
            }
            if (!init_err) {
                 high_bandwidth = bandwidth_nodes[num_unique-1].bandwidth;
                 init_err = set_closest_numanode(num_unique, bandwidth_nodes,
                                                high_bandwidth, numcpu,
                                                closest_numanode);
            }
            if (bandwidth_nodes) {
                je_free(bandwidth_nodes);
                bandwidth_nodes = NULL;
            }
            if (bandwidth) {
                je_free(bandwidth);
                bandwidth = NULL;
            }
            if (init_err) {
                if (closest_numanode) {
                    je_free(closest_numanode);
                    closest_numanode = NULL;
                }
                is_init = 0;
            }
            else {
                is_init = 1;
            }
            pthread_mutex_unlock(&init_mutex);
        }
    }
    if (!init_err && nodemask) {
        numa_bitmask_clearall(&nodemask_bm);
        cpu = sched_getcpu();
        if (cpu < numcpu) {
            numa_bitmask_setbit(&nodemask_bm, closest_numanode[cpu]);
        }
        else {
            return NUMAKIND_ERROR_GETCPU;
        }
    }
    return init_err;
}

static int parse_node_bandwidth(int num_bandwidth, int *bandwidth,
               const char *bandwidth_path)
{
    FILE *fid;
    size_t nread;
    int err = 0;
    fid = fopen(bandwidth_path, "r");
    if (!fid) {
        err = NUMAKIND_ERROR_PMTT;
    }
    if (!err) {
        nread = fread(bandwidth, sizeof(int), num_bandwidth, fid);
        if (nread != num_bandwidth) {
           err = NUMAKIND_ERROR_PMTT;
        }
    }
    return err;
}

static int create_bandwidth_nodes(int num_bandwidth, const int *bandwidth,
               int *num_unique, struct bandwidth_nodes_t **bandwidth_nodes)
{
    /***************************************************************************
    *   num_bandwidth (IN):                                                    *
    *       number of numa nodes and length of bandwidth vector.               *
    *   bandwidth (IN):                                                        *
    *       A vector of length num_bandwidth that gives bandwidth for          *
    *       each numa node, zero if numa node has unknown bandwidth.           *
    *   num_unique (OUT):                                                      *
    *       number of unique non-zero bandwidth values in bandwidth            *
    *       vector.                                                            *
    *   bandwidth_nodes (OUT):                                                 *
    *       A list of length num_unique sorted by bandwidth value where        *
    *       each element gives a list of the numa nodes that have the          *
    *       given bandwidth.                                                   *
    *   RETURNS zero on success, error code on failure                         *
    ***************************************************************************/
    int err = 0;
    int i, j, k, l, last_bandwidth;
    struct numanode_bandwidth_t *numanode_bandwidth = NULL;
    *bandwidth_nodes = NULL;
    /* allocate space for sorting array */
    numanode_bandwidth = je_malloc(sizeof(struct numanode_bandwidth_t) *
                                   num_bandwidth);
    if (!numanode_bandwidth) {
        err = NUMAKIND_ERROR_MALLOC;
    }
    if (!err) {
        /* set sorting array */
        j = 0;
        for (i = 0; i < num_bandwidth; ++i) {
            if (bandwidth[i] != 0) {
                numanode_bandwidth[j].numanode = i;
                numanode_bandwidth[j].bandwidth = bandwidth[i];
                ++j;
            }
        }
        /* ignore zero bandwidths */
        num_bandwidth = j;
        if (num_bandwidth == 0) {
            err = NUMAKIND_ERROR_HBW;
        }
    }
    if (!err) {
        qsort(numanode_bandwidth, num_bandwidth,
              sizeof(struct numanode_bandwidth_t), numanode_bandwidth_compare);
        /* calculate the number of unique bandwidths */
        *num_unique = 1;
        last_bandwidth = numanode_bandwidth[0].bandwidth;
        for (i = 1; i < num_bandwidth; ++i) {
            if (numanode_bandwidth[i].bandwidth != last_bandwidth) {
                last_bandwidth = numanode_bandwidth[i].bandwidth;
                ++*num_unique;
            }
        }
        /* allocate output array */
        *bandwidth_nodes = (struct bandwidth_nodes_t*)je_malloc(
            sizeof(struct bandwidth_nodes_t) * *num_unique +
            sizeof(int) * num_bandwidth);
        if (!*bandwidth_nodes) {
            err = NUMAKIND_ERROR_MALLOC;
        }
    }
    if (!err) {
        /* populate output */
        (*bandwidth_nodes)[0].numanodes = (int*)(*bandwidth_nodes + *num_unique);
        last_bandwidth = numanode_bandwidth[0].bandwidth;
        k = 0;
        l = 0;
        for (i = 0; i < num_bandwidth; ++i, ++l) {
            (*bandwidth_nodes)[0].numanodes[i] = numanode_bandwidth[i].numanode;
            if (numanode_bandwidth[i].bandwidth != last_bandwidth) {
                (*bandwidth_nodes)[k].num_numanodes = l;
                (*bandwidth_nodes)[k].bandwidth = last_bandwidth;
                l = 0;
                ++k;
                (*bandwidth_nodes)[k].numanodes = (*bandwidth_nodes)[0].numanodes + i;
                last_bandwidth = numanode_bandwidth[i].bandwidth;
            }
        }
        (*bandwidth_nodes)[k].num_numanodes = l;
    }
    if (numanode_bandwidth) {
        je_free(numanode_bandwidth);
    }
    if (err) {
        if (*bandwidth_nodes) {
            je_free(*bandwidth_nodes);
        }
    }
    return err;
}

static int set_closest_numanode(int num_unique,
               const struct bandwidth_nodes_t *bandwidth_nodes,
               int target_bandwidth, int num_cpunode, int *closest_numanode)
{
    /***************************************************************************
    *   num_unique (IN):                                                       *
    *       Length of bandwidth_nodes vector.                                  *
    *   bandwidth_nodes (IN):                                                  *
    *       Output vector from create_bandwitdth_nodes().                      *
    *   target_bandwidth (IN):                                                 *
    *       The bandwidth to select for comparison.                            *
    *   num_cpunode (IN):                                                      *
    *       Number of cpu's and length of closest_numanode.                    *
    *   closest_numanode (OUT):                                                *
    *       Vector that maps cpu index to closest numa node of the specified   *
    *       bandwidth.                                                         *
    *   RETURNS zero on success, error code on failure                         *
    ***************************************************************************/

    int err = 0;
    int min_distance, distance, i, j;
    struct bandwidth_nodes_t match;
    match.bandwidth = -1;
    for (i = 0; i < num_cpunode; ++i) {
        closest_numanode[i] = -1;
    }
    for (i = 0; i < num_unique; ++i) {
        if (bandwidth_nodes[i].bandwidth == target_bandwidth) {
            match = bandwidth_nodes[i];
            break;
        }
    }
    if (match.bandwidth == -1) {
        err = NUMAKIND_ERROR_HBW;
    }
    else {
        for (i = 0; i < num_cpunode; ++i) {
            min_distance = INT_MAX;
            for (j = 0; j < match.num_numanodes; ++j) {
                distance = numa_distance(numa_node_of_cpu(i),
                                         match.numanodes[j]);
                if (distance < min_distance) {
                    min_distance = distance;
                    closest_numanode[i] = match.numanodes[j];
                }
                else if (distance == min_distance) {
                    err = NUMAKIND_ERROR_TIEDISTANCE;
                }
            }
        }
    }
    return err;
}

static int numanode_bandwidth_compare(const void *a, const void *b)
{
    /***************************************************************************
    *  qsort comparison function for numa_node_bandwidth structures.  Sorts in *
    *  order of bandwidth and then numanode.                                   *
    ***************************************************************************/
    struct numanode_bandwidth_t *aa = (struct numanode_bandwidth_t *)(a);
    struct numanode_bandwidth_t *bb = (struct numanode_bandwidth_t *)(b);
    if (aa->bandwidth == bb->bandwidth) {
        if (aa->numanode > bb->numanode) {
            return 1;
        }
        else if (aa->numanode < bb->numanode) {
            return -1;
        }
        else {
            return 0;
        }
    }
    else {
        if (aa->bandwidth > bb->bandwidth) {
            return 1;
        }
        else {
            return -1;
        }
    }
}
