/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <arm64_isa_level.h>
#include <cpu_type.h>

#include <TestSuiteAddon.h>
#include <cppunit/TestFixture.h>
#include <cppunit/extensions/HelperMacros.h>


#if defined(__i386__) || defined(__x86_64__)

class CpuTypeTests : public CppUnit::TestFixture {
	CPPUNIT_TEST_SUITE(CpuTypeTests);
	CPPUNIT_TEST(NameWithInitialSpaces_ParseIntel_ReturnsNameWithoutSpaces);
	CPPUNIT_TEST(NameWithVendor_ParseIntel_ReturnsNameWithoutVendor);
	CPPUNIT_TEST(NameWithRegisteredMarkVendor_ParseIntel_ReturnsNameWithoutVendorAndMark);
	CPPUNIT_TEST(NameWithTextRAndTM_ParseIntel_ReplacesWithUTF8);
	CPPUNIT_TEST(NameWithCPUProcessor_ParseIntel_RemovesWords);
	CPPUNIT_TEST(NameWithAtRemainder_ParseIntel_RemovesRemainder);
	CPPUNIT_TEST(NameWithDuplicateSpacesInTheMiddle_ParseIntel_RemovesDuplicates);
	CPPUNIT_TEST(NameWithDuplicateSpacesAtTheEnd_ParseIntel_RemovesDuplicates);
	CPPUNIT_TEST(NameWithMHzWithoutAtCharBefore_ParseIntel_RemovesClockValue);
	CPPUNIT_TEST(NameWithGHzWithoutAtCharBefore_ParseIntel_RemovesClockValue);
	CPPUNIT_TEST(NameWithDecimalWithoutAtCharBefore_ParseIntel_RemovesClockValue);
	CPPUNIT_TEST(NameWithEverything_ParseIntel_ReturnsExpectedName);
	CPPUNIT_TEST_SUITE_END();

public:
	void NameWithInitialSpaces_ParseIntel_ReturnsNameWithoutSpaces()
	{
		const char* name = "     test";
		const char* expected = "test";
		const char* result = parse_intel(name);
		CPPUNIT_ASSERT_EQUAL(0, strcmp(expected, result));
	}

	void NameWithVendor_ParseIntel_ReturnsNameWithoutVendor()
	{
		const char* name = "Intel Core 2";
		const char* expected = "Core 2";
		const char* result = parse_intel(name);
		CPPUNIT_ASSERT_EQUAL(0, strcmp(expected, result));
	}

	void NameWithRegisteredMarkVendor_ParseIntel_ReturnsNameWithoutVendorAndMark()
	{
		const char* name = "Intel(R) Core 2";
		const char* expected = "Core 2";
		const char* result = parse_intel(name);
		CPPUNIT_ASSERT_EQUAL(0, strcmp(expected, result));
	}

	void NameWithTextRAndTM_ParseIntel_ReplacesWithUTF8()
	{
		const char* name = "Core(R) Centrino(TM) Ultra";
		const char* expected = "Core® Centrino™ Ultra";
		const char* result = parse_intel(name);
		CPPUNIT_ASSERT_EQUAL(0, strcmp(expected, result));
	}

	void NameWithCPUProcessor_ParseIntel_RemovesWords()
	{
		const char* name = "Core CPU 2 processor";
		const char* expected = "Core 2";
		const char* result = parse_intel(name);
		CPPUNIT_ASSERT_EQUAL(0, strcmp(expected, result));
	}

	void NameWithAtRemainder_ParseIntel_RemovesRemainder()
	{
		const char* name = "Core 2 @ 3 GHz per core";
		const char* expected = "Core 2";
		const char* result = parse_intel(name);
		CPPUNIT_ASSERT_EQUAL(0, strcmp(expected, result));
	}

	void NameWithDuplicateSpacesInTheMiddle_ParseIntel_RemovesDuplicates()
	{
		const char* name = "Core        2   Duo";
		const char* expected = "Core 2 Duo";
		const char* result = parse_intel(name);
		CPPUNIT_ASSERT_EQUAL(0, strcmp(expected, result));
	}

	void NameWithDuplicateSpacesAtTheEnd_ParseIntel_RemovesDuplicates()
	{
		const char* name = "Core 2      ";
		const char* expected = "Core 2";
		const char* result = parse_intel(name);
		CPPUNIT_ASSERT_EQUAL(0, strcmp(expected, result));
	}

	void NameWithMHzWithoutAtCharBefore_ParseIntel_RemovesClockValue()
	{
		const char* name = "Celeron M 900MHz";
		const char* expected = "Celeron M";
		const char* result = parse_intel(name);
		CPPUNIT_ASSERT_EQUAL(0, strcmp(expected, result));
	}

	void NameWithGHzWithoutAtCharBefore_ParseIntel_RemovesClockValue()
	{
		const char* name = "Pentium D 2GHz";
		const char* expected = "Pentium D";
		const char* result = parse_intel(name);
		CPPUNIT_ASSERT_EQUAL(0, strcmp(expected, result));
	}

	void NameWithDecimalWithoutAtCharBefore_ParseIntel_RemovesClockValue()
	{
		const char* name = "Celeron M 1.5GHz";
		const char* expected = "Celeron M";
		const char* result = parse_intel(name);
		CPPUNIT_ASSERT_EQUAL(0, strcmp(expected, result));
	}

	void NameWithEverything_ParseIntel_ReturnsExpectedName()
	{
		const char* name = "Intel(R) Celeron(R) CPU Core(TM)      1500MHz @ 1.5GHz";
		const char* expected = "Celeron® Core™";
		const char* result = parse_intel(name);
		CPPUNIT_ASSERT_EQUAL(0, strcmp(expected, result));
	}
};


CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(CpuTypeTests, getTestSuiteName());
#endif


//! The MIDR decoding is plain data handling, so it is tested on every arch.
class Arm64CpuTypeTests : public CppUnit::TestFixture {
	CPPUNIT_TEST_SUITE(Arm64CpuTypeTests);
	CPPUNIT_TEST(KnownPart_ModelString_ReturnsCoreName);
	CPPUNIT_TEST(UnknownPartOfKnownVendor_ModelString_ReturnsHexPart);
	CPPUNIT_TEST(UnknownImplementer_ModelString_ReturnsHexImplementerAndPart);
	CPPUNIT_TEST(NewVendors_VendorString_ReturnsNames);
	CPPUNIT_TEST(NoFeatures_IsaLevel_ReturnsV80);
	CPPUNIT_TEST(CortexA72Registers_IsaLevel_ReturnsV80);
	CPPUNIT_TEST(LseRdmDpb_IsaLevel_ReturnsV82);
	CPPUNIT_TEST(V84WithoutBti_IsaLevel_StopsAtV84);
	CPPUNIT_TEST(LaterLevelWithGap_IsaLevel_StopsAtGap);
	CPPUNIT_TEST(V85WithSve2_IsaLevel_ReturnsV90);
	CPPUNIT_TEST(Levels_IsaString_FormatsArmName);
	CPPUNIT_TEST_SUITE_END();

public:
	void KnownPart_ModelString_ReturnsCoreName()
	{
		// Raspberry Pi 4: Cortex-A72 r0p3
		CPPUNIT_ASSERT_EQUAL(0, strcmp("Cortex-A72", get_cpu_model_string(
			B_CPU_ARM_64, B_CPU_VENDOR_ARM, 0x410fd083)));
		CPPUNIT_ASSERT_EQUAL(0, strcmp("M1 Max Firestorm", get_cpu_model_string(
			B_CPU_ARM_64, B_CPU_VENDOR_APPLE, 0x611f0290)));
	}

	void UnknownPartOfKnownVendor_ModelString_ReturnsHexPart()
	{
		CPPUNIT_ASSERT_EQUAL(0, strcmp("part 0x000", get_cpu_model_string(
			B_CPU_ARM_64, B_CPU_VENDOR_APPLE, 0x61000000)));
	}

	void UnknownImplementer_ModelString_ReturnsHexImplementerAndPart()
	{
		CPPUNIT_ASSERT_EQUAL(0, strcmp("Implementer 0x4e part 0x004",
			get_cpu_model_string(B_CPU_ARM_64, B_CPU_VENDOR_UNKNOWN,
				0x4e0f0040)));
	}

	void NewVendors_VendorString_ReturnsNames()
	{
		CPPUNIT_ASSERT_EQUAL(0, strcmp("Fujitsu",
			get_cpu_vendor_string(B_CPU_VENDOR_FUJITSU)));
		CPPUNIT_ASSERT_EQUAL(0, strcmp("Arm",
			get_cpu_vendor_string(B_CPU_VENDOR_ARM)));
		CPPUNIT_ASSERT_EQUAL(0, strcmp("Apple",
			get_cpu_vendor_string(B_CPU_VENDOR_APPLE)));
		CPPUNIT_ASSERT_EQUAL(0, strcmp("Qualcomm",
			get_cpu_vendor_string(B_CPU_VENDOR_QUALCOMM)));
	}

	void NoFeatures_IsaLevel_ReturnsV80()
	{
		arm64_id_regs regs = {};
		CPPUNIT_ASSERT_EQUAL((uint32)0x0800, arm64_isa_level(&regs));
	}

	void CortexA72Registers_IsaLevel_ReturnsV80()
	{
		// Raspberry Pi 4: AES+PMULL, SHA1, SHA2, CRC32 only
		arm64_id_regs regs = {};
		regs.isar0 = 0x11120;
		regs.pfr0 = 0x11112222;
		CPPUNIT_ASSERT_EQUAL((uint32)0x0800, arm64_isa_level(&regs));
	}

	void LseRdmDpb_IsaLevel_ReturnsV82()
	{
		arm64_id_regs regs = {};
		regs.isar0 = 0x10200000;	// RDM 1, Atomic 2
		regs.isar1 = 0x1;			// DPB 1
		CPPUNIT_ASSERT_EQUAL((uint32)0x0802, arm64_isa_level(&regs));
	}

	void V84WithoutBti_IsaLevel_StopsAtV84()
	{
		arm64_id_regs regs = _V85();
		regs.pfr1 = 0;	// no FEAT_BTI
		CPPUNIT_ASSERT_EQUAL((uint32)0x0804, arm64_isa_level(&regs));
	}

	void LaterLevelWithGap_IsaLevel_StopsAtGap()
	{
		// 8.6 features without FEAT_JSCVT (8.3) must not count
		arm64_id_regs regs = _V85();
		regs.isar1 &= ~(0xfull << 12);
		regs.isar1 |= (1ull << 44) | (1ull << 52);
		CPPUNIT_ASSERT_EQUAL((uint32)0x0802, arm64_isa_level(&regs));
	}

	void V85WithSve2_IsaLevel_ReturnsV90()
	{
		arm64_id_regs regs = _V85();
		CPPUNIT_ASSERT_EQUAL((uint32)0x0805, arm64_isa_level(&regs));
		regs.pfr0 |= 1ull << 32;	// SVE
		regs.zfr0 = 1;				// SVE2
		CPPUNIT_ASSERT_EQUAL((uint32)0x0900, arm64_isa_level(&regs));
	}

	void Levels_IsaString_FormatsArmName()
	{
		CPPUNIT_ASSERT_EQUAL(0, strcmp("ARMv8.4-A",
			get_cpu_isa_string(B_CPU_ARM_64, 0x0804)));
		CPPUNIT_ASSERT_EQUAL(0, strcmp("ARMv9.0-A",
			get_cpu_isa_string(B_CPU_ARM_64, 0x0900)));
		CPPUNIT_ASSERT(get_cpu_isa_string(B_CPU_ARM_64, 0) == NULL);
		CPPUNIT_ASSERT(get_cpu_isa_string(B_CPU_x86_64, 0x0804) == NULL);
	}

private:
	//! Every feature of levels 8.1 to 8.5, nothing beyond.
	static arm64_id_regs _V85()
	{
		arm64_id_regs regs = {};
		// TS 2, DP 1, RDM 1, Atomic 2
		regs.isar0 = (2ull << 52) | (1ull << 44) | (1ull << 28) | (2ull << 20);
		// SB, FRINTTS, LRCPC 2, FCMA, JSCVT, API, DPB 2
		regs.isar1 = (1ull << 36) | (1ull << 32) | (2ull << 20) | (1ull << 16)
			| (1ull << 12) | (1ull << 8) | 2;
		regs.mmfr2 = 1ull << 32;	// AT (LSE2)
		regs.pfr0 = 1ull << 48;		// DIT
		regs.pfr1 = 1;				// BT
		return regs;
	}
};


CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(Arm64CpuTypeTests, getTestSuiteName());
