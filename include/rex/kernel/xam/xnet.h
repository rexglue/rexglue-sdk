/**
 * @file        kernel/xam/xnet.h
 * @brief       System link (LAN) networking shared by the XNet calls and the
 *              XGI session messages.
 *
 * The console's secure network layer (XNet) gives every peer an XNADDR and
 * encrypts traffic with session keys. Between PCs there is no Xbox Live and
 * no encryption: an XNADDR holds the PC's real LAN IPv4 address, and the
 * IN_ADDR the game sends to is that address. Keys are kept only so the game
 * gets back what it registered.
 */
#pragma once

#include <cstdint>

#include <rex/types.h>

namespace rex::kernel::xam {

// Guest structures, laid out (big-endian) as on the console.
struct X_XNADDR {
  rex::be<uint32_t> ina;          // the PC's IPv4 address
  rex::be<uint32_t> ina_online;   // Xbox Live address: always 0
  rex::be<uint16_t> port_online;  // Xbox Live port: always 0
  uint8_t enet[6];                // the console's MAC address; here made from `ina`
  uint8_t online[20];             // Xbox Live identification: always 0
};
static_assert(sizeof(X_XNADDR) == 36, "XNADDR is 36 bytes");

struct X_XNKID {
  uint8_t ab[8];
};
struct X_XNKEY {
  uint8_t ab[16];
};

struct X_XSESSION_INFO {
  X_XNKID session_id;
  X_XNADDR host_address;
  X_XNKEY key_exchange_key;
};
static_assert(sizeof(X_XSESSION_INFO) == 60, "XSESSION_INFO is 60 bytes");

namespace xnet {

// True when [address, address + length) is mapped guest memory the host may
// read (or write, with `write`). A null address is never valid.
bool GuestRangeValid(uint32_t address, uint32_t length, bool write);

// This PC's system link address (what XNetGetTitleXnAddr returns).
void GetTitleXnAddr(X_XNADDR* out);

// A new random system link key pair (XNetCreateKey).
void CreateKey(X_XNKID* kid, X_XNKEY* key);

// Registers a key pair (XNetRegisterKey). Returns 0 or a Winsock error code.
uint32_t RegisterKey(const X_XNKID& kid, const X_XNKEY& key);

}  // namespace xnet
}  // namespace rex::kernel::xam
