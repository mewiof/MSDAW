#include "PrecompHeader.h"
#include <gtest/gtest.h>

#include "ProcessorFactory.h"

// ================================================================
// PROCESSOR FACTORY
// ================================================================

// NOTE: this suite exists to catch a link-time failure, not a logic one.
// REGISTER_PROCESSOR plants a file-scope ProcessorRegistrar that no symbol
// ever references, so if MSDAWCore is ever changed from an OBJECT library to
// a static archive the linker drops those objects, the factory comes up
// empty, and the app quietly loses its built-in devices. these tests fail
// loudly instead
TEST(ProcessorFactory, BuiltInsSelfRegister) {
	const auto& factory = ProcessorFactory::Instance();

	EXPECT_TRUE(factory.IsRegistered("BitCrusher"));
	EXPECT_TRUE(factory.IsRegistered("AutoSidechain"));
}

TEST(ProcessorFactory, CreatesRegisteredProcessorsAndNullsUnknownOnes) {
	const auto& factory = ProcessorFactory::Instance();

	EXPECT_NE(factory.Create("BitCrusher"), nullptr);
	EXPECT_EQ(factory.Create("NoSuchProcessor"), nullptr);
}

// each Create hands back a fresh instance; sharing one would let two tracks
// fight over the same parameter values
TEST(ProcessorFactory, CreateReturnsDistinctInstances) {
	const auto& factory = ProcessorFactory::Instance();

	auto first = factory.Create("BitCrusher");
	auto second = factory.Create("BitCrusher");

	ASSERT_NE(first, nullptr);
	ASSERT_NE(second, nullptr);
	EXPECT_NE(first, second);
}

TEST(ProcessorFactory, RegisteredIdsAreReported) {
	const auto& factory = ProcessorFactory::Instance();

	const auto ids = factory.GetRegisteredProcessorIds();

	EXPECT_NE(std::find(ids.begin(), ids.end(), "BitCrusher"), ids.end());
	EXPECT_NE(std::find(ids.begin(), ids.end(), "AutoSidechain"), ids.end());
}

// registration is first-owner-wins, so a duplicate id must be refused rather
// than silently shadowing the built-in
TEST(ProcessorFactory, DuplicateRegistrationIsRefused) {
	auto& factory = ProcessorFactory::Instance();

	const bool registered = factory.Register(
		"BitCrusher",
		[]() -> std::shared_ptr<AudioProcessor> { return nullptr; },
		false);

	EXPECT_FALSE(registered);
	EXPECT_NE(factory.Create("BitCrusher"), nullptr);
}

// both built-ins are effects; an instrument misfiled as an effect lands in the
// wrong half of the browser and cannot be added to a track
TEST(ProcessorFactory, BuiltInEffectsAreNotInstruments) {
	const auto& factory = ProcessorFactory::Instance();

	EXPECT_FALSE(factory.IsInstrument("BitCrusher"));
	EXPECT_FALSE(factory.IsInstrument("AutoSidechain"));
}
