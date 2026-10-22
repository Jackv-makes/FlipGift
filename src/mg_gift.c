// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nicolas Aubry

#include "mg_gift.h"
#include "mg_timing.h" // MG_VERSION_*

#include <string.h>

// Names mirror MysteryGiftItems (data/items/mystery_gift_items.asm), by index.
static const char* const mg_item_names[MG_ITEM_COUNT] = {
    "Berry",       "PrzCureBerry", "Mint Berry",  "Ice Berry",    "Burnt Berry",
    "PsnCureBerry", "Guard Spec.", "X Defend",    "X Attack",     "Bitter Berry",
    "Dire Hit",    "X Special",    "X Accuracy",  "Eon Mail",     "Morph Mail",
    "Music Mail",  "MiracleBerry", "Gold Berry",  "Revive",       "Great Ball",
    "Super Repel", "Max Repel",    "Elixer",      "Ether",        "Water Stone",
    "Fire Stone",  "Leaf Stone",   "Thunderstone", "Max Ether",   "Max Elixer",
    "Max Revive",  "Scope Lens",   "HP Up",       "PP Up",        "Rare Candy",
    "BlueSky Mail", "Mirage Mail",
};

const char* mg_item_name(uint8_t index) {
    return (index < MG_ITEM_COUNT) ? mg_item_names[index] : "?";
}

// Names mirror MysteryGiftDecos (data/decorations/mystery_gift_decos.asm).
static const char* const mg_deco_names[MG_DECO_COUNT] = {
    "Jigglypuff Doll", "Poliwag Doll",  "Diglett Doll",   "Staryu Doll",     "Magikarp Doll",
    "Oddish Doll",     "Gengar Doll",   "Shellder Doll",  "Grimer Doll",     "Voltorb Doll",
    "Clefairy Poster", "Jigglyp.Poster", "SNES",          "Weedle Doll",     "Geodude Doll",
    "Machop Doll",     "Magna Plant",   "Tropic Plant",   "Famicom",         "N64",
    "Bulbasaur Doll",  "Squirtle Doll", "Pink Bed",       "Polkadot Bed",    "Red Carpet",
    "Blue Carpet",     "Yellow Carpet", "Green Carpet",   "Jumbo Plant",     "Virtual Boy",
    "Big Onix Doll",   "Pikachu Poster", "Big Lapras Doll", "Surf Pika Doll", "Pikachu Bed",
    "Unown Doll",      "Tentacool Doll",
};

const char* mg_deco_name(uint8_t index) {
    return (index < MG_DECO_COUNT) ? mg_deco_names[index] : "?";
}

char mg_gen2_to_ascii(uint8_t code) {
    if(code >= 0x80 && code <= 0x99) return (char)('A' + (code - 0x80));
    if(code >= 0xA0 && code <= 0xB9) return (char)('a' + (code - 0xA0));
    if(code >= 0xF6) return (char)('0' + (code - 0xF6)); // 0xF6..0xFF
    switch(code) {
    case 0x7F: return ' ';
    case 0xE7: return '!';
    case 0xE6: return '?';
    case 0xE3: return '-';
    case 0xE8: return '.';
    case 0x9A: return '(';
    case 0x9B: return ')';
    case 0x9C: return ':';
    case 0x9D: return ';';
    case 0x9E: return '[';
    case 0x9F: return ']';
    default: return '?';
    }
}

bool mg_gift_parse_payload1(const uint8_t* buf, uint8_t len, MgPartnerInfo* out) {
    if(!buf || !out || len < MG_PAYLOAD1_LEN) return false;
    memset(out, 0, sizeof(*out));
    out->version = buf[P1_OFF_VERSION];
    out->trainer_id = ((uint16_t)buf[P1_OFF_TID_HI] << 8) | buf[P1_OFF_TID_LO];
    uint8_t n = 0;
    for(uint8_t i = 0; i < P1_NAME_LEN; i++) {
        uint8_t c = buf[P1_OFF_NAME + i];
        if(c == 0x50 || c == 0x00) break;
        out->trainer_name[n++] = mg_gen2_to_ascii(c);
    }
    out->trainer_name[n] = '\0';
    out->pokedex_count = buf[P1_OFF_DEX_COUNT];
    out->sent_deco = buf[P1_OFF_SENT_DECO];
    out->which_item = buf[P1_OFF_WHICH_ITEM];
    out->which_deco = buf[P1_OFF_WHICH_DECO];
    out->backup_item = buf[P1_OFF_BACKUP_ITEM];
    out->counter = buf[P1_OFF_COUNTER];
    return true;
}

uint8_t mg_gift_parse_payload2(const uint8_t* buf, uint8_t len, MgPartyMon* out) {
    if(!buf || !out) return 0;
    uint8_t count = 0;
    for(uint8_t off = 0; count < MG_PARTY_MAX && off + MG_PARTY_MON_SIZE <= len; off += MG_PARTY_MON_SIZE) {
        if(buf[off] == 0xFF || buf[off] == 0x00) break; // terminator / padding
        out[count].level = buf[off];
        out[count].species = buf[off + 1];
        out[count].move1 = buf[off + 2];
        out[count].move2 = buf[off + 3];
        out[count].move3 = buf[off + 4];
        out[count].move4 = buf[off + 5];
        count++;
    }
    return count;
}

// Gen II text encoding (subset). Terminator 0x50, space 0x7F.
uint8_t mg_gen2_char(char c) {
    if(c >= 'A' && c <= 'Z') return 0x80 + (c - 'A');
    if(c >= 'a' && c <= 'z') return 0xA0 + (c - 'a');
    if(c >= '0' && c <= '9') return 0xF6 + (c - '0');
    if(c == ' ') return 0x7F;
    if(c == '!') return 0xE7;
    if(c == '?') return 0xE6;
    return 0x7F; // unmapped -> space
}

static void mg_write_name(uint8_t* dst, const char* name, uint8_t field_len) {
    uint8_t i = 0;
    if(name) {
        for(; name[i] != '\0' && i < field_len; i++) {
            dst[i] = mg_gen2_char(name[i]);
        }
    }
    if(i < field_len) dst[i++] = 0x50; // terminator
    for(; i < field_len; i++) dst[i] = 0x00; // pad
}

uint8_t mg_gift_build_payload1(uint8_t* buf, const MgGiftInfo* info) {
    memset(buf, 0, MG_PAYLOAD1_LEN);
    buf[P1_OFF_VERSION] = info->version;
    buf[P1_OFF_TID_HI] = (uint8_t)(info->trainer_id >> 8); // big-endian
    buf[P1_OFF_TID_LO] = (uint8_t)(info->trainer_id & 0xFF);
    mg_write_name(&buf[P1_OFF_NAME], info->trainer_name, P1_NAME_LEN);
    buf[P1_OFF_DEX_COUNT] = info->pokedex_count;
    buf[P1_OFF_SENT_DECO] = info->sent_deco;
    buf[P1_OFF_WHICH_ITEM] = info->which_item;
    buf[P1_OFF_WHICH_DECO] = info->which_deco;
    buf[P1_OFF_BACKUP_ITEM] = info->backup_item; // 0 => "friend is ready"
    buf[P1_OFF_COUNTER] = info->counter;
    return MG_PAYLOAD1_LEN;
}

uint8_t mg_gift_build_payload2(uint8_t* buf, const MgPartyMon* mons, uint8_t count) {
    if(count > MG_PARTY_MAX) count = MG_PARTY_MAX;
    memset(buf, 0, MG_PAYLOAD2_LEN);
    uint8_t n = 0;
    for(uint8_t i = 0; i < count; i++) {
        buf[n++] = mons[i].level;
        buf[n++] = mons[i].species;
        buf[n++] = mons[i].move1;
        buf[n++] = mons[i].move2;
        buf[n++] = mons[i].move3;
        buf[n++] = mons[i].move4;
    }
    buf[n++] = 0xFF; // -1 terminator after the last mon
    // Remaining bytes stay 0 (padding). Total is fixed at 38.
    return MG_PAYLOAD2_LEN;
}

MgGiftInfo mg_gift_default_item(const char* trainer_name, uint16_t tid, uint8_t item_index) {
    MgGiftInfo g = {
        .version = MG_VERSION_GOLD_CRYSTAL, // 0x01
        .trainer_id = tid,
        .trainer_name = trainer_name,
        .pokedex_count = 30,
        .sent_deco = MG_SENT_ITEM,
        .which_item = item_index,
        .which_deco = 0,
        .backup_item = 0,
        .counter = 0,
    };
    return g;
}
