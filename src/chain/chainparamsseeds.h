#ifndef MAGNACHAIN_CHAINPARAMSSEEDS_H
#define MAGNACHAIN_CHAINPARAMSSEEDS_H
/**
* List of fixed seed nodes for the magnachain network
* AUTOGENERATED by contrib/seeds/generate-seeds.py
*
* Each line contains a 16-byte IPv6 address and a port.
* IPv4 as well as onion addresses are wrapped inside a IPv6 address accordingly.
*/
static SeedSpec6 pnSeed6_main[] = {
//	{ { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xff,0xff,0xc0,0xa8,0x3b,0x80 }, 8333 }
#ifdef _MSC_VER // msvc cannot allocate an array of constant size 0
	{ { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 }, 8333 }
#endif
};

static SeedSpec6 pnSeed6_test[] = {
//	{ { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xff,0xff,0xc0,0xa8,0x3b,0x80 }, 8333 }
#ifdef _MSC_VER // msvc cannot allocate an array of constant size 0
	{ { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 }, 8333 }
#endif
};
#endif // MAGNACHAIN_CHAINPARAMSSEEDS_H
