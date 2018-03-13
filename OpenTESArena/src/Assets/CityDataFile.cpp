#include <algorithm>
#include <cassert>
#include <cmath>

#include "CityDataFile.h"
#include "../Utilities/Bytes.h"
#include "../Utilities/Debug.h"

#include "components/vfs/manager.hpp"

const int CityDataFile::PROVINCE_COUNT = 9;

int CityDataFile::getGlobalCityID(int localCityID, int provinceID)
{
	return (provinceID << 5) + localCityID;
}

std::pair<int, int> CityDataFile::getLocalCityAndProvinceID(int globalCityID)
{
	return std::make_pair(globalCityID & 0x1F, globalCityID >> 5);
}

int CityDataFile::getDistance(const Int2 &p1, const Int2 &p2)
{
	const int dx = std::abs(p1.x - p2.x);
	const int dy = std::abs(p1.y - p2.y);
	return std::max(dx, dy) + (std::min(dx, dy) / 4);
}

Int2 CityDataFile::localPointToGlobal(const Int2 &localPoint, const Rect &rect)
{
	const int globalX = ((localPoint.x * ((rect.getWidth() * 100) / 320)) / 100) + rect.getLeft();
	const int globalY = ((localPoint.y * ((rect.getHeight() * 100) / 200)) / 100) + rect.getTop();
	return Int2(globalX, globalY);
}

Int2 CityDataFile::globalPointToLocal(const Int2 &globalPoint, const Rect &rect)
{
	const int localX = ((globalPoint.x - rect.getLeft()) * 100) / ((rect.getWidth() * 100) / 320);
	const int localY = ((globalPoint.y - rect.getTop()) * 100) / ((rect.getHeight() * 100) / 200);
	return Int2(localX, localY);
}

std::string CityDataFile::getMainQuestDungeonMifName(uint32_t seed)
{
	const std::string seedString = std::to_string(seed);
	const std::string mifName = seedString.substr(0, 8) + ".MIF";
	return mifName;
}

const CityDataFile::ProvinceData &CityDataFile::getProvinceData(int index) const
{
	assert(index < CityDataFile::PROVINCE_COUNT);
	return this->provinces.at(index);
}

int CityDataFile::getGlobalQuarter(const Int2 &globalPoint)
{
	Rect provinceRect;

	// Find the province that contains the global point.
	const auto iter = std::find_if(this->provinces.begin(), this->provinces.end(),
		[&globalPoint, &provinceRect](const CityDataFile::ProvinceData &province)
	{
		provinceRect = Rect(province.globalX, province.globalY,
			province.globalW, province.globalH);
		return provinceRect.contains(globalPoint);
	});

	DebugAssert(iter != this->provinces.end(), "No matching province for global point (" +
		std::to_string(globalPoint.x) + ", " + std::to_string(globalPoint.y) + ").");

	const Int2 localPoint = CityDataFile::globalPointToLocal(globalPoint, provinceRect);
	const int provinceID = static_cast<int>(std::distance(this->provinces.begin(), iter));

	// Get the global quarter index.
	const int globalQuarter = [&localPoint, provinceID]()
	{
		int index = provinceID * 4;
		const bool inRightHalf = localPoint.x >= 160;
		const bool inBottomHalf = localPoint.y >= 100;

		// Add to the index depending on which quadrant the local point is in.
		if (inRightHalf)
		{
			index++;
		}

		if (inBottomHalf)
		{
			index += 2;
		}

		return index;
	}();

	return globalQuarter;
}

uint32_t CityDataFile::getDungeonSeed(int dungeonID, int provinceID) const
{
	const auto &province = this->provinces.at(provinceID);
	const auto &dungeon = [dungeonID, &province]()
	{
		if (dungeonID == 0)
		{
			// Second main quest dungeon.
			return province.secondDungeon;
		}
		else if (dungeonID == 1)
		{
			// First main quest dungeon.
			return province.firstDungeon;
		}
		else
		{
			return province.randomDungeons.at(dungeonID - 2);
		}
	}();

	const uint32_t seed = (dungeon.y << 16) + dungeon.x + provinceID;
	return (~Bytes::rol32(seed, 5)) & 0xFFFFFFFF;
}

uint32_t CityDataFile::getWildernessDungeonSeed(int provinceID,
	int wildBlockX, int wildBlockY) const
{
	const auto &province = this->provinces.at(provinceID);
	const uint32_t baseSeed = ((province.globalX << 16) + province.globalY) * provinceID;
	return (baseSeed + (((wildBlockY << 6) + wildBlockX) & 0xFFFF)) & 0xFFFFFFFF;
}

void CityDataFile::init(const std::string &filename)
{
	VFS::IStreamPtr stream = VFS::Manager::get().open(filename);
	DebugAssert(stream != nullptr, "Could not open \"" + filename + "\".");

	stream->seekg(0, std::ios::end);
	std::vector<uint8_t> srcData(stream->tellg());
	stream->seekg(0, std::ios::beg);
	stream->read(reinterpret_cast<char*>(srcData.data()), srcData.size());

	// Size of each province definition in bytes.
	const size_t provinceDataSize = 1228;

	// Size of each location definition in bytes.
	const size_t locationDataSize = 25;

	// Iterate over each province and initialize the location data.
	for (size_t i = 0; i < this->provinces.size(); i++)
	{
		const size_t startOffset = provinceDataSize * i;
		auto &province = this->provinces.at(i);

		// Read the province header.
		const uint8_t *provinceNamePtr = srcData.data() + startOffset;
		std::copy(provinceNamePtr, provinceNamePtr + province.name.size(), province.name.begin());

		const uint8_t *provinceGlobalDimsPtr = provinceNamePtr + province.name.size();
		province.globalX = Bytes::getLE16(provinceGlobalDimsPtr);
		province.globalY = Bytes::getLE16(provinceGlobalDimsPtr + sizeof(province.globalX));
		province.globalW = Bytes::getLE16(provinceGlobalDimsPtr + (sizeof(province.globalX) * 2));
		province.globalH = Bytes::getLE16(provinceGlobalDimsPtr + (sizeof(province.globalX) * 3));

		const uint8_t *locationPtr = provinceGlobalDimsPtr + (sizeof(province.globalX) * 4);

		for (auto &cityState : province.cityStates)
		{
			// Read the city-state data.
			std::copy(locationPtr, locationPtr + cityState.name.size(), cityState.name.begin());
			cityState.x = Bytes::getLE16(locationPtr + cityState.name.size());
			cityState.y = Bytes::getLE16(locationPtr + cityState.name.size() + sizeof(cityState.x));
			cityState.visibility = *(locationPtr + cityState.name.size() +
				sizeof(cityState.x) + sizeof(cityState.y));
			locationPtr += locationDataSize;
		}

		for (auto &town : province.towns)
		{
			// Read the town data.
			std::copy(locationPtr, locationPtr + town.name.size(), town.name.begin());
			town.x = Bytes::getLE16(locationPtr + town.name.size());
			town.y = Bytes::getLE16(locationPtr + town.name.size() + sizeof(town.x));
			town.visibility = *(locationPtr + town.name.size() + sizeof(town.x) + sizeof(town.y));
			locationPtr += locationDataSize;
		}

		for (auto &village : province.villages)
		{
			// Read the village data.
			std::copy(locationPtr, locationPtr + village.name.size(), village.name.begin());
			village.x = Bytes::getLE16(locationPtr + village.name.size());
			village.y = Bytes::getLE16(locationPtr + village.name.size() + sizeof(village.x));
			village.visibility = *(locationPtr + village.name.size() +
				sizeof(village.x) + sizeof(village.y));
			locationPtr += locationDataSize;
		}

		// Read the dungeon data. The second dungeon is listed first.
		std::copy(locationPtr, locationPtr + province.secondDungeon.name.size(),
			province.secondDungeon.name.begin());
		province.secondDungeon.x = Bytes::getLE16(locationPtr + province.secondDungeon.name.size());
		province.secondDungeon.y = Bytes::getLE16(locationPtr +
			province.secondDungeon.name.size() + sizeof(province.secondDungeon.x));
		province.secondDungeon.visibility = *(locationPtr + province.secondDungeon.name.size() +
			sizeof(province.secondDungeon.x) + sizeof(province.secondDungeon.y));
		locationPtr += locationDataSize;

		std::copy(locationPtr, locationPtr + province.firstDungeon.name.size(),
			province.firstDungeon.name.begin());
		province.firstDungeon.x = Bytes::getLE16(locationPtr + province.firstDungeon.name.size());
		province.firstDungeon.y = Bytes::getLE16(locationPtr +
			province.firstDungeon.name.size() + sizeof(province.firstDungeon.x));
		province.firstDungeon.visibility = *(locationPtr + province.firstDungeon.name.size() +
			sizeof(province.firstDungeon.x) + sizeof(province.firstDungeon.y));
		locationPtr += locationDataSize;

		// Read random dungeon data.
		for (auto &dungeon : province.randomDungeons)
		{
			// Read the random dungeon data.
			std::copy(locationPtr, locationPtr + dungeon.name.size(), dungeon.name.begin());
			dungeon.x = Bytes::getLE16(locationPtr + dungeon.name.size());
			dungeon.y = Bytes::getLE16(locationPtr + dungeon.name.size() + sizeof(dungeon.x));
			dungeon.visibility = *(locationPtr + dungeon.name.size() +
				sizeof(dungeon.x) + sizeof(dungeon.y));
			locationPtr += locationDataSize;
		}
	}
}
