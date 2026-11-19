#include "mrc_wr.h"
#include "mrc_qp.h"

#include <errno.h>
#include <string.h>

#define NO_STAGE SIZE_MAX

bool mrc_wr_init(struct mrc_wr_responder *responder, const struct mrc_wr_config *config)
{
    if (!responder || !config || !config->pmtu ||
        config->initial_psn > MRC_PSN_MASK || config->initial_msn > MRC_PSN_MASK ||
        !config->work || !config->work_capacity || config->work_capacity > MRC_MPR_MAX ||
        config->max_wimm_inflight > config->work_capacity ||
        (!config->pd && (config->region.length > UINT64_MAX - config->region.address ||
                         (config->region.length && !config->region.memory))) ||
        config->staged_capacity > MRC_MPR_MAX ||
        config->staged_capacity > SIZE_MAX / config->pmtu ||
        (config->staged_capacity && (!config->staged || !config->staged_payload)) ||
        (config->recv_capacity && !config->recv_queue))
        return false;

    if (config->pd && mrc_provider_pd_retain(config->pd))
        return false;

    memset(config->work, 0, config->work_capacity * sizeof(*config->work));
    for (size_t index = 0; index < config->staged_capacity; index++) {
        config->staged[index].next =
            index + 1 == config->staged_capacity ? NO_STAGE : index + 1;
    }
    *responder = (struct mrc_wr_responder) {
        .config = *config,
        .next_psn = config->initial_psn,
        .next_msn = config->initial_msn,
        .next_rqmsn = config->initial_rqmsn,
        .free_stage = config->staged_capacity ? 0 : NO_STAGE,
    };
    return true;
}

static void release_work(struct mrc_wr_slot *slot)
{
    if (slot->lease.owner)
        mrc_provider_mr_release(&slot->lease);
    slot->message = (struct mrc_wr_message){0};
}

static void release_all_work(struct mrc_wr_responder *responder)
{
    for (size_t i = 0; i < responder->config.work_capacity; i++)
        release_work(&responder->config.work[i]);
}

void mrc_wr_destroy(struct mrc_wr_responder *responder)
{
    if (!responder)
        return;
    release_all_work(responder);
    if (responder->config.pd)
        mrc_provider_pd_release(responder->config.pd);
    *responder = (struct mrc_wr_responder){ .failed = true };
}

bool mrc_wr_post_receive(struct mrc_wr_responder *responder, uint64_t wr_id)
{
    size_t capacity = responder->config.recv_capacity;
    if (responder->failed || responder->recv_posted == capacity)
        return false;

    size_t index = (responder->recv_head + responder->recv_posted) % capacity;
    responder->config.recv_queue[index] = wr_id;
    responder->recv_posted++;
    return true;
}

static enum mrc_wr_result fail(struct mrc_wr_responder *responder, enum mrc_wr_result error)
{
    release_all_work(responder);
    responder->failed = true;
    return error;
}

static bool within_region(const struct mrc_wr_region *region, uint64_t va, size_t length)
{
    if (va < region->address || va - region->address > region->length)
        return false;
    return length <= region->length - (va - region->address);
}

static bool could_fit_middle(uint64_t region_iova, size_t region_length, uint32_t pmtu,
                             uint64_t va, uint32_t packets, uint32_t dmalen)
{
    if (dmalen > region_length)
        return false;

    uint64_t offset = va - region_iova;
    uint64_t latest_base = region_length - dmalen;
    uint64_t first_index = offset > latest_base ? 1 + (offset - latest_base - 1) / pmtu : 1;
    return first_index <= packets - 2 && first_index <= offset / pmtu;
}

static int acquire_write(struct mrc_provider_pd *pd, uint32_t key, uint64_t va, size_t length,
                         struct mrc_provider_mr_lease *lease)
{
    if (!key)
        return -EACCES;
    const struct mrc_provider_mr_access access = {
        .iova = va, .length = length, .key = key, .operation = MRC_PROVIDER_MR_WRITE_REMOTE,
    };
    return mrc_provider_mr_acquire(pd, &access, lease);
}

static int validate_unanchored(const struct mrc_wr_responder *responder,
                               const struct mrc_wire_write *write, uint32_t packets)
{
    struct mrc_provider_mr_lease lease = {0};
    int result = acquire_write(responder->config.pd, write->r_key, write->va,
                               write->data_len, &lease);
    if (!result) {
        if (!could_fit_middle(lease.region_iova, lease.region_length, responder->config.pmtu,
                              write->va, packets, write->dma_len))
            result = -EACCES;
        mrc_provider_mr_release(&lease);
    }
    return result;
}

static struct mrc_wr_slot *find_work(const struct mrc_wr_responder *responder, uint16_t msn,
                                     struct mrc_wr_slot **available)
{
    for (size_t i = 0; i < responder->config.work_capacity; i++) {
        struct mrc_wr_slot *slot = &responder->config.work[i];
        if (slot->message.used && slot->message.msn == msn)
            return slot;
        if (available && !slot->message.used && !*available)
            *available = slot;
    }
    return NULL;
}

static bool valid_position(const struct mrc_wr_message *slot, uint32_t pmtu, uint32_t psn,
                           uint64_t va, bool middle)
{
    int32_t offset = mrc_psn_delta(psn, slot->first_psn);
    if (offset < 0 || (uint32_t)offset >= slot->packets ||
        (middle && (!offset || (uint32_t)offset + 1 == slot->packets)))
        return false;

    return va == slot->base_va + (uint64_t)(uint32_t)offset * pmtu;
}

static bool covers(const struct mrc_wr_message *slot, uint32_t psn)
{
    int32_t offset = mrc_psn_delta(psn, slot->first_psn);
    return offset >= 0 && (uint32_t)offset < slot->packets;
}

static bool valid_anchor(const struct mrc_wr_responder *responder, const struct mrc_wr_message *candidate)
{
    uint32_t first = (uint32_t)mrc_psn_delta(candidate->first_psn, responder->next_psn);
    uint16_t message = (uint16_t)(candidate->msn - (uint16_t)responder->next_msn);
    if (first >= MRC_PSN_HALF || candidate->packets > MRC_PSN_HALF - first ||
        first < message || (!message && first))
        return false;

    uint32_t end = first + candidate->packets;
    for (size_t i = 0; i < responder->config.work_capacity; i++) {
        const struct mrc_wr_message *other = &responder->config.work[i].message;
        if (!other->used || other->msn == candidate->msn)
            continue;

        if (!other->anchored) {
            for (size_t index = other->staged_head; index != NO_STAGE;
                 index = responder->config.staged[index].next) {
                if (covers(candidate, responder->config.staged[index].psn))
                    return false;
            }
            continue;
        }

        uint32_t other_first = (uint32_t)mrc_psn_delta(other->first_psn, responder->next_psn);
        uint32_t other_end = other_first + other->packets;
        uint16_t other_message = (uint16_t)(other->msn - (uint16_t)responder->next_msn);
        if (message < other_message) {
            if (end > other_first || other_first - end < (uint32_t)(other_message - message - 1) ||
                (other_message == message + 1 && end != other_first))
                return false;
        } else {
            if (other_end > first || first - other_end < (uint32_t)(message - other_message - 1) ||
                (message == other_message + 1 && other_end != first))
                return false;
        }
    }
    return true;
}

static bool valid_staged(const struct mrc_wr_responder *responder, const struct mrc_wr_message *slot)
{
    for (size_t index = slot->staged_head; index != NO_STAGE;
         index = responder->config.staged[index].next) {
        const struct mrc_wr_staged *packet = &responder->config.staged[index];
        if (!valid_position(slot, responder->config.pmtu, packet->psn, packet->va, true))
            return false;
    }
    return true;
}

static void place(const struct mrc_wr_responder *responder, const struct mrc_wr_message *message,
                  const struct mrc_provider_mr_lease *lease, uint64_t va, const uint8_t *data,
                  size_t length)
{
    if (!length)
        return;
    if (responder->config.pd) {
        uint8_t *memory = lease->address;
        memcpy(memory + (va - message->base_va), data, length);
    } else {
        const struct mrc_wr_region *region = &responder->config.region;
        memcpy(region->memory + (va - region->address), data, length);
    }
}

static void place_staged(struct mrc_wr_responder *responder, struct mrc_wr_message *message,
                         const struct mrc_provider_mr_lease *lease)
{
    while (message->staged_head != NO_STAGE) {
        size_t index = message->staged_head;
        struct mrc_wr_staged *packet = &responder->config.staged[index];
        uint32_t pmtu = responder->config.pmtu;
        place(responder, message, lease, packet->va,
              responder->config.staged_payload + index * pmtu, pmtu);
        message->staged_head = packet->next;
        packet->next = responder->free_stage;
        responder->free_stage = index;
        message->placed++;
    }
}

static enum mrc_wr_result stage_middle(struct mrc_wr_responder *responder,
                                       struct mrc_wr_message *slot, uint32_t psn,
                                       const struct mrc_wire_write *write)
{
    size_t index = responder->free_stage;
    if (index == NO_STAGE)
        return MRC_WR_NO_PKT_BUFFER;

    for (size_t i = 0; i < responder->config.work_capacity; i++) {
        const struct mrc_wr_message *other = &responder->config.work[i].message;
        if (other->used && other->anchored && other->msn != slot->msn && covers(other, psn))
            return fail(responder, MRC_WR_INVALID);
    }

    struct mrc_wr_staged *packet = &responder->config.staged[index];
    responder->free_stage = packet->next;
    *packet = (struct mrc_wr_staged) { .va = write->va, .next = slot->staged_head, .psn = psn };
    memcpy(responder->config.staged_payload + index * responder->config.pmtu,
           write->data, responder->config.pmtu);
    slot->staged_head = index;
    return MRC_WR_OK;
}

enum mrc_wr_result mrc_wr_receive(struct mrc_wr_responder *responder, uint32_t psn,
                                  uint8_t opcode, const struct mrc_wire_write *write)
{
    if (responder->failed)
        return MRC_WR_QP_ERROR;
    if (!write || psn > MRC_PSN_MASK || (write->data_len && !write->data))
        return fail(responder, MRC_WR_INVALID);

    uint32_t pmtu = responder->config.pmtu;
    uint32_t packets = write->dma_len ? 1 + (write->dma_len - 1) / pmtu : 1;
    uint64_t final_offset = (uint64_t)(packets - 1) * pmtu;
    bool first = opcode == MRC_WRITE_FIRST;
    bool middle = opcode == MRC_WRITE_MIDDLE;
    bool last = opcode == MRC_WRITE_LAST || opcode == MRC_WRITE_LAST_IMM;
    bool only = opcode == MRC_WRITE_ONLY || opcode == MRC_WRITE_ONLY_IMM;
    bool immediate = opcode == MRC_WRITE_LAST_IMM || opcode == MRC_WRITE_ONLY_IMM;
    if ((!first && !middle && !last && !only) || (only != (packets == 1)) ||
        (middle && packets < 3) || packets >= MRC_PSN_HALF ||
        write->data_len != ((first || middle) ? pmtu : write->dma_len - final_offset))
        return fail(responder, MRC_WR_INVALID);

    const struct mrc_wr_region *region = &responder->config.region;
    if (!responder->config.pd &&
        (write->r_key != region->r_key || !within_region(region, write->va, write->data_len) ||
         (middle && !could_fit_middle(region->address, region->length, pmtu,
                                      write->va, packets, write->dma_len))))
        return fail(responder, MRC_WR_REMOTE_ACCESS);

    uint16_t message = (uint16_t)(write->msn - (uint16_t)responder->next_msn);
    int32_t psn_distance = mrc_psn_delta(psn, responder->next_psn);
    if (psn_distance < message || message >= UINT16_C(0x8000) ||
        (middle && psn_distance == message))
        return fail(responder, MRC_WR_INVALID);

    /* Keep a logical slot for each nearer MSN. Admitting arbitrary future
     * messages could prevent the missing prefix from ever being accepted. */
    if (message >= responder->config.work_capacity)
        return MRC_WR_NO_RESOURCE;

    struct mrc_wr_slot *available = NULL;
    struct mrc_wr_slot *slot = find_work(responder, write->msn, &available);
    if (!slot && !available)
        return MRC_WR_NO_RESOURCE;

    struct mrc_wr_slot *target = slot ? slot : available;
    struct mrc_wr_message candidate = slot ? slot->message : (struct mrc_wr_message) {
        .staged_head = NO_STAGE, .dmalen = write->dma_len, .packets = packets,
        .r_key = write->r_key, .msn = write->msn, .rqmsn = write->rqmsn,
        .used = true, .uniform_rqmsn = true,
    };
    if (candidate.r_key != write->r_key)
        return fail(responder, MRC_WR_REMOTE_ACCESS);
    if (candidate.dmalen != write->dma_len || ((last || only) && candidate.final_seen))
        return fail(responder, MRC_WR_INVALID);

    candidate.uniform_rqmsn &= candidate.rqmsn == write->rqmsn;
    if ((immediate || candidate.with_imm) && !candidate.uniform_rqmsn)
        return fail(responder, MRC_WR_INVALID);

    if (middle && !candidate.anchored) {
        if (responder->config.pd) {
            int result = validate_unanchored(responder, write, packets);
            if (result == -EOVERFLOW)
                return MRC_WR_NO_RESOURCE;
            if (result)
                return fail(responder, MRC_WR_REMOTE_ACCESS);
        }
        enum mrc_wr_result result = stage_middle(responder, &candidate, psn, write);
        if (result != MRC_WR_OK)
            return result;
    } else {
        bool needs_anchor = !candidate.anchored;
        if (!middle) {
            uint64_t offset = last ? final_offset : 0;
            if (write->va < offset || write->va - offset > UINT64_MAX - write->dma_len ||
                (!responder->config.pd &&
                 !within_region(region, write->va - offset, write->dma_len)))
                return fail(responder, MRC_WR_REMOTE_ACCESS);

            uint32_t first_psn = mrc_psn_add(psn, last ? -(int32_t)(packets - 1) : 0);
            uint64_t base_va = write->va - offset;
            if (candidate.anchored &&
                (candidate.first_psn != first_psn || candidate.base_va != base_va))
                return fail(responder, MRC_WR_INVALID);

            if (needs_anchor) {
                candidate.anchored = true;
                candidate.base_va = base_va;
                candidate.first_psn = first_psn;
                if (!valid_anchor(responder, &candidate) || !valid_staged(responder, &candidate))
                    return fail(responder, MRC_WR_INVALID);
            }
        } else if (!valid_position(&candidate, pmtu, psn, write->va, true)) {
            return fail(responder, MRC_WR_INVALID);
        }

        if (last || only) {
            if (immediate && responder->wimm_inflight == responder->config.max_wimm_inflight)
                return fail(responder, MRC_WR_IMM_LIMIT);
            candidate.final_seen = true;
            candidate.with_imm = immediate;
            candidate.immediate = write->imm_data;
        }
        if (needs_anchor && responder->config.pd) {
            int result = acquire_write(responder->config.pd, candidate.r_key, candidate.base_va,
                                       candidate.dmalen, &target->lease);
            if (result == -EOVERFLOW)
                return MRC_WR_NO_RESOURCE;
            if (result)
                return fail(responder, MRC_WR_REMOTE_ACCESS);
        }

        responder->wimm_inflight += immediate;
        place(responder, &candidate, &target->lease, write->va, write->data, write->data_len);
        candidate.placed++;
        place_staged(responder, &candidate, &target->lease);
    }

    target->message = candidate;
    return MRC_WR_OK;
}

enum mrc_wr_result mrc_wr_advance(struct mrc_wr_responder *responder, uint32_t delivery_cack,
                                  struct mrc_wr_completion *completion, bool *produced)
{
    *produced = false;
    if (responder->failed)
        return MRC_WR_QP_ERROR;
    if (delivery_cack > MRC_PSN_MASK)
        return fail(responder, MRC_WR_INVALID);

    while (mrc_psn_delta(delivery_cack, responder->next_psn) >= 0) {
        struct mrc_wr_slot *work = find_work(responder, (uint16_t)responder->next_msn, NULL);
        if (!work)
            return fail(responder, MRC_WR_INVALID);
        const struct mrc_wr_message *slot = &work->message;
        if (!slot->anchored || slot->first_psn != responder->next_psn)
            return fail(responder, MRC_WR_INVALID);

        uint32_t last = mrc_psn_add(slot->first_psn, (int32_t)(slot->packets - 1));
        if (mrc_psn_delta(delivery_cack, last) < 0)
            break;
        if (!slot->final_seen || slot->placed != slot->packets)
            return fail(responder, MRC_WR_INVALID);

        if (slot->with_imm) {
            if (slot->rqmsn != responder->next_rqmsn)
                return fail(responder, MRC_WR_INVALID);
            if (!responder->recv_posted)
                return fail(responder, MRC_WR_RQ_EMPTY);

            *completion = (struct mrc_wr_completion) {
                .wr_id = responder->config.recv_queue[responder->recv_head],
                .msn = responder->next_msn, .last_psn = last,
                .byte_len = slot->dmalen, .immediate = slot->immediate,
            };
            responder->recv_head = (responder->recv_head + 1) % responder->config.recv_capacity;
            responder->recv_posted--;
            responder->wimm_inflight--;
            responder->next_rqmsn++;
            *produced = true;
        }
        responder->next_psn = mrc_psn_add(last, 1);
        responder->next_msn = mrc_psn_add(responder->next_msn, 1);
        release_work(work);
        if (*produced)
            break;
    }

    return MRC_WR_OK;
}

uint32_t mrc_wr_ack_msn(const struct mrc_wr_responder *responder)
{
    return mrc_psn_add(responder->next_msn, -1);
}
