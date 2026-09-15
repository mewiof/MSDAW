#include "PrecompHeader.h"
#include <gtest/gtest.h>

#include <filesystem>

#include "GridScale.h"
#include "Project.h"

// ================================================================
// ADAPTIVE GRID
// ================================================================

// the grid left to follow the zoom picks the finest musical division still wide enough
// to see and to hit. these pin down the shape of that choice rather than the exact
// numbers, which kMinCellPixels is free to move

TEST(GridScale, ZoomingInRefinesTheDivision) {
	const double wide = GridScale::Adaptive(400.0f);
	const double narrow = GridScale::Adaptive(20.0f);

	// more pixels per beat buys a finer grid, never a coarser one
	EXPECT_LT(wide, narrow);
}

TEST(GridScale, EveryCellIsWideEnoughToAimAt) {
	for (float pixelsPerBeat = 2.0f; pixelsPerBeat < 2000.0f; pixelsPerBeat *= 1.35f) {
		const double grid = GridScale::Adaptive(pixelsPerBeat);
		const double cellPixels = grid * pixelsPerBeat;

		// the coarsest rung is the floor: below that zoom there is nothing wider to pick
		if (grid < 16.0)
			EXPECT_GE(cellPixels, (double)GridScale::kMinCellPixels) << "at " << pixelsPerBeat << " px/beat";
	}
}

TEST(GridScale, TheDivisionIsAlwaysMusical) {
	for (float pixelsPerBeat = 2.0f; pixelsPerBeat < 2000.0f; pixelsPerBeat *= 1.35f) {
		const double grid = GridScale::Adaptive(pixelsPerBeat);

		// a power of two either side of the beat, so the lines land on bars and on
		// divisions notes are actually written in
		const double log2Grid = std::log2(grid);
		EXPECT_NEAR(log2Grid, std::round(log2Grid), 1e-9) << "grid " << grid;
	}
}

TEST(GridScale, AHighDpiScreenAsksForAWiderCell) {
	const float pixelsPerBeat = 100.0f;

	// the minimum is a physical size, not a pixel count: at 200% a cell needs twice the
	// pixels to be the same target, so the division can only get coarser
	EXPECT_GE(GridScale::Adaptive(pixelsPerBeat, 2.0f), GridScale::Adaptive(pixelsPerBeat, 1.0f));
}

TEST(GridScale, TheAutoFlagSurvivesAProjectSaveAndLoad) {
	const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
	std::filesystem::path path = std::filesystem::temp_directory_path() /
								 (std::string("msdaw-") + info->name() + ".msdaw");
	std::filesystem::remove(path);

	{
		Project saved;
		saved.Initialize();
		ProjectViewState vs = saved.GetViewState();
		vs.timelineGridAuto = true;
		vs.timelineGridNumerator = 1;
		vs.timelineGridDenominator = 16;
		saved.SetViewState(vs);
		saved.Save(path.string());
	}

	Project loaded;
	loaded.Load(path.string());
	EXPECT_TRUE(loaded.GetViewState().timelineGridAuto);
	// the fixed division is remembered too - it is what switching Auto off goes back to
	EXPECT_EQ(loaded.GetViewState().timelineGridDenominator, 16);

	std::filesystem::remove(path);
}

TEST(GridScale, AProjectWithoutTheFlagReadsAsAFixedGrid) {
	Project project;
	project.Initialize();

	// older projects carry no VIEW_GRID_AUTO line at all, and must not start adapting
	EXPECT_FALSE(project.GetViewState().timelineGridAuto);
}
