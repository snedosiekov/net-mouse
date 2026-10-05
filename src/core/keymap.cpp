#include "core/keymap.h"

#include <cstddef>

namespace nm::keymap {
namespace {

// Windows virtual-key codes (winuser.h), kept numeric so this file stays platform independent.
constexpr uint32_t kVkPause = 0x13;
constexpr uint32_t kVkSnapshot = 0x2C;
constexpr uint32_t kVkSleep = 0x5F;
constexpr uint32_t kVkNumLock = 0x90;

struct Pair {
  uint16_t win;  // scan code (E0-prefixed table) or virtual-key code (vk table)
  uint16_t code;
};

// Keys with an E0 prefix (extended flag set in the hook).
constexpr Pair kExtended[] = {
    {0x1C, 96},  /* KP Enter */    {0x1D, 97},  /* Right Ctrl */  {0x35, 98},  /* KP / */
    {0x37, 99},  /* Print Screen */ {0x38, 100}, /* Right Alt */  {0x47, 102}, /* Home */
    {0x48, 103}, /* Up */          {0x49, 104}, /* Page Up */     {0x4B, 105}, /* Left */
    {0x4D, 106}, /* Right */       {0x4F, 107}, /* End */         {0x50, 108}, /* Down */
    {0x51, 109}, /* Page Down */   {0x52, 110}, /* Insert */      {0x53, 111}, /* Delete */
    {0x5B, 125}, /* Left Win */    {0x5C, 126}, /* Right Win */   {0x5D, 127}, /* Menu */
    {0x20, 113}, /* Mute */        {0x2E, 114}, /* Volume Down */ {0x30, 115}, /* Volume Up */
    {0x22, 164}, /* Play/Pause */  {0x24, 166}, /* Stop */        {0x10, 165}, /* Previous */
    {0x19, 163}, /* Next */        {0x6A, 158}, /* Browser Back */ {0x69, 159}, /* Browser Forward */
    {0x67, 173}, /* Refresh */     {0x32, 172}, /* Home page */   {0x6C, 155}, /* Mail */
    {0x21, 140}, /* Calculator */
};

// Keys that are matched and injected by virtual-key code (their scan codes are unusual).
constexpr Pair kByVirtualKey[] = {
    {kVkPause, 119},    {kVkSnapshot, 99},  {kVkNumLock, 69},   {kVkSleep, 142},
    {0xAD, 113} /* mute */, {0xAE, 114},    {0xAF, 115},        {0xB0, 163} /* next */,
    {0xB1, 165} /* prev */, {0xB2, 166},    {0xB3, 164},        {0xA6, 158} /* back */,
    {0xA7, 159},        {0xA8, 173},        {0xAC, 172},        {0xB4, 155} /* mail */,
    {0xB7, 140} /* calc */,
};

// Plain (non-extended) scan codes beyond the identity range.
constexpr Pair kPlainExtra[] = {
    {0x54, 99},                                                     // Alt+SysRq
    {0x64, 183}, {0x65, 184}, {0x66, 185}, {0x67, 186}, {0x68, 187}, {0x69, 188},  // F13-F18
    {0x6A, 189}, {0x6B, 190}, {0x6C, 191}, {0x6D, 192}, {0x6E, 193}, {0x76, 194},  // F19-F24
    {0x70, 93},  /* Katakana/Hiragana */ {0x73, 89}, /* Ro */ {0x79, 92}, /* Henkan */
    {0x7B, 94},  /* Muhenkan */ {0x7D, 124}, /* Yen */
};

template <size_t N>
uint16_t findCode(const Pair (&table)[N], uint32_t win) {
  for (const Pair& p : table) {
    if (p.win == win) return p.code;
  }
  return 0;
}

template <size_t N>
uint16_t findWin(const Pair (&table)[N], uint16_t code) {
  for (const Pair& p : table) {
    if (p.code == code) return p.win;
  }
  return 0;
}

}  // namespace

uint16_t fromWindows(uint32_t vk, uint32_t scan, bool extended) {
  if (scan & 0x200) return 0;  // fake Left Ctrl sent along with AltGr
  if (vk == kVkPause || vk == kVkNumLock || vk == kVkSnapshot) return findCode(kByVirtualKey, vk);
  scan &= 0xFF;
  if (scan == 0) return findCode(kByVirtualKey, vk);  // e.g. media keys from some drivers
  if (extended) {
    const uint16_t code = findCode(kExtended, scan);
    return code ? code : findCode(kByVirtualKey, vk);
  }
  // Set-1 scan codes 1..0x58 equal the evdev codes (Esc = 1, A = 30, F12 = 88, ...).
  if (scan >= 1 && scan <= 0x58 && scan != 0x54) return static_cast<uint16_t>(scan);
  return findCode(kPlainExtra, scan);
}

bool toWindows(uint16_t code, WinKey& out) {
  out = {};
  // Keys injected by virtual-key code. NumLock needs the extended flag to be told apart from Pause.
  if (const uint16_t vk = findWin(kByVirtualKey, code)) {
    out.vk = vk;
    out.extended = code == 69;
    return true;
  }
  if (const uint16_t scan = findWin(kExtended, code)) {
    out.scan = scan;
    out.extended = true;
    return true;
  }
  if (code >= 1 && code <= 0x58 && code != 0x54) {
    out.scan = code;
    return true;
  }
  if (const uint16_t scan = findWin(kPlainExtra, code)) {
    out.scan = scan;
    return true;
  }
  return false;
}

}  // namespace nm::keymap
