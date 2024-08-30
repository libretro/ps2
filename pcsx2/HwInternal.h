#pragma once

// hw read functions
template< uint page > extern uint8_t  hwRead8  (u32 mem);
template< uint page > extern uint16_t hwRead16 (u32 mem);
template< uint page > extern uint32_t hwRead32 (u32 mem);
template< uint page > extern uint64_t hwRead64 (u32 mem);
template< uint page > extern RETURNS_R128       hwRead128(u32 mem);

extern uint16_t hwRead16_page_0F_INTC_HACK(u32 mem);
extern uint32_t hwRead32_page_0F_INTC_HACK(u32 mem);

// hw write functions
template<uint page> extern void hwWrite8  (u32 mem, u8  value);
template<uint page> extern void hwWrite16 (u32 mem, u16 value);

template<uint page> extern void hwWrite32 (u32 mem, uint32_t value);
template<uint page> extern void hwWrite64 (u32 mem, uint64_t srcval);
template<uint page> extern void TAKES_R128 hwWrite128(u32 mem, r128 srcval);

// --------------------------------------------------------------------------------------
//  Hardware FIFOs (128 bit access only!)
// --------------------------------------------------------------------------------------
// VIF0   -- 0x10004000 -- eeHw[0x4000]
// VIF1   -- 0x10005000 -- eeHw[0x5000]
// GIF    -- 0x10006000 -- eeHw[0x6000]
// IPUout -- 0x10007000 -- eeHw[0x7000]
// IPUin  -- 0x10007010 -- eeHw[0x7010]

extern void ReadFIFO_VIF1(u128* out);
extern void ReadFIFO_IPUout(u128* out);

extern void WriteFIFO_VIF0(const u128* value);
extern void WriteFIFO_VIF1(const u128* value);
extern void WriteFIFO_GIF(const u128* value);
extern void WriteFIFO_IPUin(const u128* value);
