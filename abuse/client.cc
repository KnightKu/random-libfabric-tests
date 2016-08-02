#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <netinet/in.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdarg.h>
#include <assert.h>

#include <cstdlib>
#include <cinttypes>

#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_eq.h>
#include <rdma/fi_rma.h>
#include <rdma/fi_errno.h>

#include "abuse.h"


// Mapping of [0, num_servers) to their MCW ranks
static int *server_mcw_ranks = NULL;


//
// If we have an entry in the fi_addr_t cache for a given MCW rank,
// then we're "connected" (meaning: we have some state about this
// peer).
//
static bool client_rank_is_connected(int rank)
{
    rank_to_fi_addr_map_t::iterator it;
    it = client_rank_to_fi_addr_map.find(rank);
    if (client_rank_to_fi_addr_map.end() == it) {
        logme("=== client NOT already connected to MCW %d!\n", rank);
        return false;
    } else {
        logme("=== client already connected to MCW %d\n", rank);
        return true;
    }
    //return (client_rank_to_fi_addr_map.end() == it) ? false : true;
}

static void client_rank_disconnect(int rank)
{
    rank_to_fi_addr_map_t::iterator it;
    it = client_rank_to_fi_addr_map.find(rank);
    if (client_rank_to_fi_addr_map.end() != it) {
        client_rank_to_fi_addr_map.erase(it);
    }
}

//
// Look up a cached fi_addr_t based on an MPI_COMM_WORLD rank.  If we
// don't have it, fi_av_insert() it, and then save the result in the
// cache map for next time.
//
static fi_addr_t client_rank_to_fi_addr(int rank)
{
    if (client_rank_is_connected(rank)) {
        return client_rank_to_fi_addr_map[rank];
    }

    assert(modex_is_done);

    struct sockaddr_in addr_sin;
    memset(&addr_sin, 0, sizeof(addr_sin));
    addr_sin.sin_family = AF_INET;
    addr_sin.sin_addr.s_addr = modex_data[rank].ip_addr.ip_addr;
    addr_sin.sin_port = modex_data[rank].ip_addr.ip_port_be;

    int ret;
    fi_addr_t addr_fi;
    ret = fi_av_insert(fidev.av, &addr_sin, 1, &addr_fi, 0, NULL);
    assert(1 == ret);
    logme(" %s:%d --> fi_addr_insert of fi_addr_t 0x%" PRIx64 "\n",
          inet_ntoa(addr_sin.sin_addr), htons(addr_sin.sin_port), addr_fi);

    client_rank_to_fi_addr_map[rank] = addr_fi;
    assert(client_rank_is_connected(rank));

    return addr_fi;
}

static void client_wait_for_recv(endpoint_t &ep, msg_type_t type,
                                 cqe_context_t *&cqec)
{
    logme("Waiting to receive message of type %d\n", type);

    // Wait for something on the CQ
    struct fi_cq_msg_entry cqe;
    while (1) {
        wait_for_cq(ep.cq, cqe);

        assert(cqe.op_context);
        cqec = (cqe_context_t*) cqe.op_context;

        // If we completed a send, free the buffer and cqec
        if (cqe.flags & FI_SEND) {
            logme("CQ: Completed a send\n");
            delete cqec->buffer;
            delete cqec;
            continue;
        }

        // If we completed an RDMA (should never happen here in the client)
        if ((cqe.flags & FI_RECV) && (cqe.flags & FI_RMA)) {
            logme("CQ: Completed an RDMA\n");
            continue;
        }

        // Make sure it was a receive
        assert(cqe.flags & FI_RECV);
        assert(cqe.flags & FI_MSG);
        logme("CQ: Completed a receive\n");

        assert(cqec->type == CQEC_RECV);
        assert(cqec->seq == 0);

        // Make sure the received message was the right type
        msg_t *msg;
        msg = (msg_t*) cqec->buffer;
        msg_verify(ep, msg);
        assert(msg->type == type);

        // All good!
        return;
    }
}

static void client_hulk_smash(endpoint_t &ep)
{
    // JMS This seems to be problematic, but we're also running into
    // other problems.  So temporarily disable the
    // tear-everything-down-and-rebuild-from-scratch stuff.
    return;



















    logme("Hulk smash!\n");

    // Tear it all down
    teardown_ofi_rdma_slab(ep);
    teardown_ofi_endpoint(ep);
    teardown_ofi_device();

    // Recreate
    setup_ofi_device();
    setup_ofi_endpoint(ep);
    setup_ofi_rdma_slab(ep);
}

static void client_connect(endpoint_t &ep, int server_mcw_rank)
{
    log_outbound_msg(ep, server_mcw_rank, "MSG_CONNECT");

    assert(modex_is_done);
    assert(!client_rank_is_connected(server_mcw_rank));

    // Fill header of message
    msg_t *to_server = new msg_t;
    assert(to_server);
    msg_fill_header(ep, to_server, MSG_CONNECT);
    to_server->to_ip = modex_data[server_mcw_rank].ip_addr;

    // Fill payload of message
    to_server->u.connect.client_rdma_key = fi_mr_key(ep.rdma_slab_mr);
    logme("MSG_CONNECT sending my slab RDMA key: %" PRIx64 "\n",
          fi_mr_key(ep.rdma_slab_mr));

    // Send
    fi_addr_t peer_fi;
    peer_fi = client_rank_to_fi_addr(server_mcw_rank);
    msg_send(ep, to_server, peer_fi);

    // Wait for CONNECT_ACK message and completion of the send
    cqe_context_t *cqec;
    logme("Waiting to receive CONNECT_ACK...\n");
    client_wait_for_recv(ep, MSG_CONNECT_ACK, cqec);
    logme("Received CONNECT_ACK -- yay!\n");

    // We don't need any information from the CONNECT_ACK -- just
    // getting it is good enough.

    // Repost the receive
    post_receive(ep, cqec->buffer, cqec);
}

static void client_solicit_rdma(endpoint_t &ep, int server_mcw_rank)
{
    // If we're not already connected, connect
    if (!client_rank_is_connected(server_mcw_rank)) {
        client_connect(ep, server_mcw_rank);
    }

    log_outbound_msg(ep, server_mcw_rank, "MSG_SOLICIT_RDMA");

    // Reset the RDMA slab to known values
    memset(ep.rdma_slab, 17, RDMA_SLAB_SIZE);

    // Ask for an RDMA to a random chunk in the middle of the slab
    size_t offset = (size_t) ((RDMA_SLAB_SIZE / 2) * drand48());
    // Make sure it is aligned
    offset -= (offset % 8);
    size_t len = (size_t) ((RDMA_SLAB_SIZE / 2) * drand48());
    uint8_t *ptr = ep.rdma_slab + offset;
    assert(ptr < (ep.rdma_slab + RDMA_SLAB_SIZE));
    assert(ptr + len < (ep.rdma_slab + RDMA_SLAB_SIZE));

    assert(modex_is_done);

    // Request some RDMA from the server
    msg_t *to_server = new msg_t;
    assert(to_server);
    msg_fill_header(ep, to_server, MSG_SOLICIT_RDMA);
    to_server->to_ip = modex_data[server_mcw_rank].ip_addr;
    logme("SOLICIT_RDMA buffer: %p - %p, len: %" PRIu64 ", key: 0x%" PRIx64 "\n",
          ptr, (ptr + len), len, fi_mr_key(ep.rdma_slab_mr));

    // Fill payload of message
    to_server->u.solicit_rdma.client_rdma_target_addr = (uint64_t)(uintptr_t) ptr;
    to_server->u.solicit_rdma.client_rdma_target_len = (uint64_t) len;

    // Send it
    fi_addr_t peer_fi;
    peer_fi = client_rank_to_fi_addr(server_mcw_rank);
    msg_send(ep, to_server, peer_fi);

    // Wait for a message back from the server saying that the RDMA to
    // my slab is done
    cqe_context_t *cqec;
    client_wait_for_recv(ep, MSG_RDMA_SENT, cqec);

    // Make sure we got a valid reply from the server
    msg_t *from_server;
    from_server = (msg_t*) cqec->buffer;
    assert(from_server->u.rdma_sent.client_rdma_target_addr ==
           (uint64_t)(uintptr_t) ptr);
    assert(from_server->u.rdma_sent.client_rdma_actual_len <= len);

    logme("Got MSG_RDMA_SENT: server wrote %" PRIu64 " bytes to %p\n",
          from_server->u.rdma_sent.client_rdma_actual_len,
          from_server->u.rdma_sent.client_rdma_target_addr);

    // Check that we got what we think we should have gotten
    buffer_pattern_check(ptr, from_server->u.rdma_sent.client_rdma_actual_len);

    // Repost the receive
    post_receive(ep, cqec->buffer, cqec);
}

static void client_disconnect(endpoint_t &ep, int server_mcw_rank)
{
    // If we're not connected, just return
    if (!client_rank_is_connected(server_mcw_rank)) {
        return;
    }

    log_outbound_msg(ep, server_mcw_rank, "MSG_DISCONNECT");

    assert(modex_is_done);

    // Fill header of message
    msg_t *to_server = new msg_t;
    assert(to_server);
    msg_fill_header(ep, to_server, MSG_DISCONNECT);
    to_server->to_ip = modex_data[server_mcw_rank].ip_addr;

    // There is no payload for the DISCONNECT message

    // Send and wait for the message
    fi_addr_t peer_fi;
    peer_fi = client_rank_to_fi_addr(server_mcw_rank);
    msg_send(ep, to_server, peer_fi);

    // Mark us as "disconnected"
    client_rank_disconnect(server_mcw_rank);

    // Remove this peer from the av
    int ret;
    ret = fi_av_remove(fidev.av, &peer_fi, 1, 0);
    assert(0 == ret);

    struct sockaddr_in addr_sin;
    memset(&addr_sin, 0, sizeof(addr_sin));
    addr_sin.sin_family = AF_INET;
    addr_sin.sin_addr.s_addr = modex_data[server_mcw_rank].ip_addr.ip_addr;
    addr_sin.sin_port = modex_data[server_mcw_rank].ip_addr.ip_port_be;
    logme("%s:%d --> fi_av_remove of fi_addr_t 0x%" PRIx64 "\n",
          inet_ntoa(addr_sin.sin_addr), htons(addr_sin.sin_port), peer_fi);
}

//
// Main Client routine
//
void client_main()
{
    setup_ofi_device();

    // Get all the servers' OFI endpoints IP addresses+ports.
    // Additionally: in the real application, all server IPs are
    // distributed OOB ahead of time, but client IPs are discovered by
    // servers when the clients send to them.
    struct sockaddr_in sin = {0};
    modex(sin);

    server_mcw_ranks = new int[comm_size];
    memset(server_mcw_ranks, 0, sizeof(int) * comm_size);
    for (int i = 0; i < comm_size; ++i) {
        if (modex_data[i].is_server) {
            server_mcw_ranks[num_servers] = i;
            ++num_servers;
        }
    }

    // Create my endpoint and RDMA slab
    endpoint_t ep;
    setup_ofi_endpoint(ep);
    setup_ofi_rdma_slab(ep);

    // Post receives
    post_receives(ep);

    // Main client loop
    double chaos;
    int server_mcw_rank;
    while (1) {
        // Randomly pick a server
        server_mcw_rank = server_mcw_ranks[rand() % num_servers];

        // Randomly pick an action
        chaos = drand48();
        if (chaos < 0.05) {
            client_hulk_smash(ep);
        } else if (chaos < 0.25) {
            client_disconnect(ep, server_mcw_rank);
        } else {
            client_solicit_rdma(ep, server_mcw_rank);
        }

        // Wait for a (short) random amount of time to simulate
        // computation
        usleep(drand48() * 100);
    }

    teardown_ofi_endpoint(ep);
    teardown_ofi_device();
}