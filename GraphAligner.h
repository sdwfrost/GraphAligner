#ifndef GraphAligner_H
#define GraphAligner_H

#include <chrono>
#include <algorithm>
#include <string>
#include <vector>
#include <cmath>
#include <unordered_set>
#include <queue>
#include <boost/config.hpp>
#include <boost/property_map/property_map.hpp>
#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/graphviz.hpp>
#include <boost/graph/johnson_all_pairs_shortest.hpp>
#include <boost/container/flat_set.hpp>
#include <boost/rational.hpp>
#include <boost/multiprecision/cpp_int.hpp>
#include "SliceRow.h"
#include "SparseBoolMatrix.h"
#include "AlignmentGraph.h"
#include "vg.pb.h"
#include "ThreadReadAssertion.h"
#include "NodeSlice.h"
#include "CommonUtils.h"

void printtime(const char* msg)
{
	static auto time = std::chrono::system_clock::now().time_since_epoch() / std::chrono::milliseconds(1);
	auto newtime = std::chrono::system_clock::now().time_since_epoch() / std::chrono::milliseconds(1);
	std::cout << msg << " " << newtime << " (" << (newtime - time) << ")" << std::endl;
	time = newtime;
}

template <typename Word>
class WordConfiguration
{
};

template <>
class WordConfiguration<uint64_t>
{
public:
	static constexpr int WordSize = 64;
	//number of bits per chunk
	//prefix sum differences are calculated in chunks of log w bits
	static constexpr int ChunkBits = 8;
	static constexpr uint64_t AllZeros = 0x0000000000000000;
	static constexpr uint64_t AllOnes = 0xFFFFFFFFFFFFFFFF;
	//positions of the sign bits for each chunk
	static constexpr uint64_t SignMask = 0x8080808080808080;
	//constant for multiplying the chunk popcounts into prefix sums
	//this should be 1 at the start of each chunk
	static constexpr uint64_t PrefixSumMultiplierConstant = 0x0101010101010101;
	//positions of the least significant bits for each chunk
	static constexpr uint64_t LSBMask = 0x0101010101010101;

	static int popcount(uint64_t x)
	{
		//https://en.wikipedia.org/wiki/Hamming_weight
		x -= (x >> 1) & 0x5555555555555555;
		x = (x & 0x3333333333333333) + ((x >> 2) & 0x3333333333333333);
		x = (x + (x >> 4)) & 0x0f0f0f0f0f0f0f0f;
		return (x * 0x0101010101010101) >> 56;
	}

	static uint64_t ChunkPopcounts(uint64_t value)
	{
		uint64_t x = value;
		x -= (x >> 1) & 0x5555555555555555;
		x = (x & 0x3333333333333333) + ((x >> 2) & 0x3333333333333333);
		x = (x + (x >> 4)) & 0x0f0f0f0f0f0f0f0f;
		return x;
	}
};

#ifndef NDEBUG
thread_local int debugLastRowMinScore;
#endif

template <typename LengthType, typename ScoreType, typename Word>
class GraphAligner
{
private:
	typedef std::pair<LengthType, LengthType> MatrixPosition;
	class MatrixSlice
	{
	public:
		std::vector<ScoreType> minScorePerWordSlice;
		std::vector<LengthType> minScoreIndexPerWordSlice;
		size_t cellsProcessed;
		LengthType FinalMinScoreDistance() const
		{
			assert(minScorePerWordSlice.size() > 0);
			assert(minScoreIndexPerWordSlice.size() == minScorePerWordSlice.size());
			return (minScoreIndexPerWordSlice.size() - 1) * WordConfiguration<Word>::WordSize;
		}
		ScoreType FinalMinScore() const
		{
			assert(minScorePerWordSlice.size() > 0);
			assert(minScoreIndexPerWordSlice.size() == minScorePerWordSlice.size());
			return minScorePerWordSlice.back();
		}
		LengthType FinalMinScoreColumn() const
		{
			assert(minScorePerWordSlice.size() > 0);
			assert(minScoreIndexPerWordSlice.size() == minScorePerWordSlice.size());
			return minScoreIndexPerWordSlice.back();
		}
	};
	class TwoDirectionalSplitAlignment
	{
	public:
		ScoreType MinScore() const
		{
			return scoresForward.back() + scoresBackward.back();
		}
		ScoreType MaxScore() const
		{
			return scoresForward.back() + scoresBackward.back() + nodeSize + startExtensionWidth * 2;
		}
		size_t sequenceSplitIndex;
		std::vector<ScoreType> scoresForward;
		std::vector<ScoreType> scoresBackward;
		std::vector<LengthType> minIndicesForward;
		std::vector<LengthType> minIndicesBackward;
		size_t nodeSize;
		size_t startExtensionWidth;
	};
	class WordSlice
	{
	public:
		WordSlice() :
		VP(WordConfiguration<Word>::AllZeros),
		VN(WordConfiguration<Word>::AllZeros),
		scoreEnd(0),
		scoreBeforeStart(0)
		{}
		WordSlice(Word VP, Word VN, ScoreType scoreEnd, ScoreType scoreBeforeStart) :
		VP(VP),
		VN(VN),
		scoreEnd(scoreEnd),
		scoreBeforeStart(scoreBeforeStart)
		{}
		Word VP;
		Word VN;
		ScoreType scoreEnd;
		ScoreType scoreBeforeStart;
	};
public:
	class AlignmentResult
	{
	public:
		AlignmentResult()
		{
		}
		AlignmentResult(vg::Alignment alignment, bool alignmentFailed, size_t cellsProcessed, size_t ms) :
		alignment(alignment),
		alignmentFailed(alignmentFailed),
		cellsProcessed(cellsProcessed),
		elapsedMilliseconds(ms)
		{
		}
		vg::Alignment alignment;
		bool alignmentFailed;
		size_t cellsProcessed;
		size_t elapsedMilliseconds;
	};

	GraphAligner(const AlignmentGraph& graph) :
	graph(graph)
	{
	}
	
	AlignmentResult AlignOneWay(const std::string& seq_id, const std::string& sequence, int dynamicWidth, LengthType dynamicRowStart) const
	{
		auto timeStart = std::chrono::system_clock::now();
		assert(graph.finalized);
		auto band = getFullBand(sequence.size(), dynamicRowStart);
		auto trace = getBacktrace(sequence, dynamicWidth, dynamicRowStart, band);
		auto timeEnd = std::chrono::system_clock::now();
		size_t time = std::chrono::duration_cast<std::chrono::milliseconds>(timeEnd - timeStart).count();
		//failed alignment, don't output
		if (std::get<0>(trace) == std::numeric_limits<ScoreType>::max()) return emptyAlignment(time, std::get<2>(trace));
		auto result = traceToAlignment(seq_id, sequence, std::get<0>(trace), std::get<1>(trace), std::get<2>(trace));
		timeEnd = std::chrono::system_clock::now();
		time = std::chrono::duration_cast<std::chrono::milliseconds>(timeEnd - timeStart).count();
		result.elapsedMilliseconds = time;
		return result;
	}

	AlignmentResult AlignOneWay(const std::string& seq_id, const std::string& sequence, int dynamicWidth, LengthType dynamicRowStart, const std::vector<std::pair<int, size_t>>& seedHits, int startBandwidth) const
	{
		auto timeStart = std::chrono::system_clock::now();
		assert(graph.finalized);
		assert(seedHits.size() > 0);
		TwoDirectionalSplitAlignment bestAlignment;
		std::pair<int, size_t> bestSeed;
		bool hasAlignment = false;
		for (size_t i = 0; i < seedHits.size(); i++)
		{
			std::cerr << "seed " << i << "/" << seedHits.size() << " " << seedHits[i].first << "," << seedHits[i].second << std::endl;
			auto result = getSplitAlignment(sequence, dynamicWidth, startBandwidth, seedHits[i].first, seedHits[i].second, hasAlignment ? bestAlignment.MaxScore() : sequence.size() * 0.4);
			if (result.MinScore() > sequence.size() * 0.4) continue;
			if (!hasAlignment)
			{
				bestAlignment = std::move(result);
				bestSeed = seedHits[i];
				hasAlignment = true;
			}
			else
			{
				if (result.MinScore() < bestAlignment.MinScore())
				{
					bestAlignment = std::move(result);
					bestSeed = seedHits[i];
				}
			}
		}
		auto timeEnd = std::chrono::system_clock::now();
		size_t time = std::chrono::duration_cast<std::chrono::milliseconds>(timeEnd - timeStart).count();
		//failed alignment, don't output
		if (!hasAlignment)
		{
			return emptyAlignment(time, 0);
		}
		auto bestTrace = getPiecewiseTracesFromSplit(bestAlignment, sequence);

		auto fwresult = traceToAlignment(seq_id, sequence, std::get<0>(bestTrace.first), std::get<1>(bestTrace.first), 0);
		auto bwresult = traceToAlignment(seq_id, sequence, std::get<0>(bestTrace.second), reverseTrace(std::get<1>(bestTrace.second)), 0);
		//failed alignment, don't output
		if (fwresult.alignmentFailed && bwresult.alignmentFailed)
		{
			return emptyAlignment(time, 0);
		}
		auto result = mergeAlignments(bwresult, fwresult);
		timeEnd = std::chrono::system_clock::now();
		time = std::chrono::duration_cast<std::chrono::milliseconds>(timeEnd - timeStart).count();
		result.elapsedMilliseconds = time;
		return result;
	}

private:

	class ExpandoCell
	{
	public:
		ExpandoCell(LengthType w, LengthType j, size_t bt) :
		position(w, j),
		backtraceIndex(bt)
		{}
		MatrixPosition position;
		size_t backtraceIndex;
	};

	std::pair<size_t, size_t> getLargestContiguousBlock(const std::vector<bool>& vec) const
	{
		size_t thisBlock = 0;
		size_t maxBlockSize = 0;
		size_t maxBlockEnd = 0;
		for (size_t i = 0; i < vec.size(); i++)
		{
			if (vec[i])
			{
				thisBlock++;
			}
			else
			{
				if (thisBlock > maxBlockSize)
				{
					assert(i > 0);
					assert(i >= thisBlock);
					maxBlockEnd = i-1;
					maxBlockSize = thisBlock - 1;
				}
				thisBlock = 0;
			}
		}
		if (thisBlock > maxBlockSize)
		{
			maxBlockEnd = vec.size()-1;
			maxBlockSize = thisBlock - 1;
		}
		assert(maxBlockEnd >= maxBlockSize);
		return std::make_pair(maxBlockEnd - maxBlockSize, maxBlockEnd);
	}

	std::pair<ScoreType, std::vector<MatrixPosition>> estimateCorrectnessAndBacktraceBiggestPart(const std::string& sequence, const std::vector<ScoreType>& minScorePerWordSlice, const std::vector<LengthType>& minScoreIndexPerWordSlice) const
	{
		auto correctParts = estimateCorrectAlignmentViterbi(minScorePerWordSlice);
		auto ends = getLargestContiguousBlock(correctParts);
		auto start = ends.first;
		auto end = ends.second;
		if (end == start) return std::make_pair(sequence.size(), std::vector<MatrixPosition>{});
		assert(end < minScoreIndexPerWordSlice.size() - 1);
		assert(minScoreIndexPerWordSlice.size() == minScorePerWordSlice.size());
		assert(end > start);
		MatrixPosition endPos = std::make_pair(minScoreIndexPerWordSlice[end + 1], (end - start + 1) * WordConfiguration<Word>::WordSize);
		assert(endPos.second <= sequence.size());
		std::string newseq = sequence.substr(start * WordConfiguration<Word>::WordSize, (end - start + 1) * WordConfiguration<Word>::WordSize);
		assert(endPos.second == newseq.size());
		std::vector<ScoreType> partials;
		assert(end + 2 <= minScorePerWordSlice.size());
		for (size_t i = start; i < end + 2; i++)
		{
			partials.push_back(minScorePerWordSlice[i]);
		}
		return backtrace(endPos, newseq, partials);
	}

	std::pair<ScoreType, std::vector<MatrixPosition>> backtrace(MatrixPosition endPosition, const std::string& sequence, const std::vector<ScoreType>& minScorePerWordSlice) const
	{
		auto result = backtraceInner(endPosition, sequence, minScorePerWordSlice);
		assert(result.second.size() != 0);
		return result;
	}

	std::pair<ScoreType, std::vector<MatrixPosition>> backtraceInner(MatrixPosition endPosition, const std::string& sequence, const std::vector<ScoreType>& minScorePerWordSlice) const
	{
		assert(minScorePerWordSlice.size() * WordConfiguration<Word>::WordSize > sequence.size());
		ScoreType scoreAtEnd = minScorePerWordSlice.back();
		ScoreType currentDistance = 0;
		std::vector<ExpandoCell> visitedExpandos;
		std::vector<ExpandoCell> currentDistanceQueue;
		std::vector<ExpandoCell> currentDistancePlusOneQueue;
		currentDistanceQueue.emplace_back(endPosition.first, endPosition.second, 0);
		SparseBoolMatrix<SliceRow<LengthType>> visitedCells {graph.nodeSequences.size(), sequence.size()+1};

		while (true)
		{
			if (currentDistanceQueue.size() == 0)
			{
				assert(currentDistancePlusOneQueue.size() > 0);
				assert(currentDistancePlusOneQueue.size() > 0);
				std::swap(currentDistanceQueue, currentDistancePlusOneQueue);
				currentDistance++;
				assert(currentDistance <= scoreAtEnd);
			}
			auto current = currentDistanceQueue.back();
			currentDistanceQueue.pop_back();
			auto w = current.position.first;
			auto j = current.position.second;
			if (j == 0)
			{
				visitedExpandos.push_back(current);
				break;
			}
			auto sliceIndex = (j-1) / WordConfiguration<Word>::WordSize;
			assert(sliceIndex < minScorePerWordSlice.size());
			ScoreType maxDistanceHere= scoreAtEnd - minScorePerWordSlice[sliceIndex];
			if (currentDistance > maxDistanceHere) continue;
			if (visitedCells.get(w, j)) continue;
			visitedCells.set(w, j);
			visitedExpandos.push_back(current);
			auto nodeIndex = graph.indexToNode[w];
			auto backtraceIndexToCurrent = visitedExpandos.size()-1;
			currentDistancePlusOneQueue.emplace_back(w, j-1, backtraceIndexToCurrent);
			if (w == graph.nodeStart[nodeIndex])
			{
				for (auto neighbor : graph.inNeighbors[nodeIndex])
				{
					auto u = graph.nodeEnd[neighbor]-1;
					currentDistancePlusOneQueue.emplace_back(u, j, backtraceIndexToCurrent);
					if (sequence[j-1] == 'N' || graph.nodeSequences[w] == sequence[j-1])
					{
						currentDistanceQueue.emplace_back(u, j-1, backtraceIndexToCurrent);
					}
					else
					{
						currentDistancePlusOneQueue.emplace_back(u, j-1, backtraceIndexToCurrent);
					}
				}
			}
			else
			{
				auto u = w-1;
				currentDistancePlusOneQueue.emplace_back(u, j, backtraceIndexToCurrent);
				if (sequence[j-1] == 'N' || graph.nodeSequences[w] == sequence[j-1])
				{
					currentDistanceQueue.emplace_back(u, j-1, backtraceIndexToCurrent);
				}
				else
				{
					currentDistancePlusOneQueue.emplace_back(u, j-1, backtraceIndexToCurrent);
				}
			}
		}
		std::cerr << "backtrace visited " << visitedCells.totalOnes() << " cells" << std::endl;
		assert(currentDistance <= scoreAtEnd);
		auto index = visitedExpandos.size()-1;
		std::vector<MatrixPosition> result;
		while (index > 0)
		{
			result.push_back(visitedExpandos[index].position);
			assert(visitedExpandos[index].backtraceIndex < index);
			index = visitedExpandos[index].backtraceIndex;
		}
		return std::make_pair(currentDistance, result);
	}

	std::vector<std::vector<bool>> getFullBand(size_t sequenceSize, LengthType dynamicRowStart) const
	{
		std::vector<std::vector<bool>> result;
		result.resize(dynamicRowStart/WordConfiguration<Word>::WordSize);
		for (size_t i = 0; i < dynamicRowStart/WordConfiguration<Word>::WordSize; i++)
		{
			result[i].resize(graph.nodeStart.size(), true);
		}
		return result;
	}

	AlignmentResult emptyAlignment(size_t elapsedMilliseconds, size_t cellsProcessed) const
	{
		vg::Alignment result;
		result.set_score(std::numeric_limits<decltype(result.score())>::max());
		return AlignmentResult { result, true, cellsProcessed, elapsedMilliseconds };
	}

	bool posEqual(const vg::Position& pos1, const vg::Position& pos2) const
	{
		return pos1.node_id() == pos2.node_id() && pos1.is_reverse() == pos2.is_reverse();
	}

	AlignmentResult mergeAlignments(const AlignmentResult& first, const AlignmentResult& second) const
	{
		assert(!first.alignmentFailed || !second.alignmentFailed);
		if (first.alignmentFailed) return second;
		if (second.alignmentFailed) return first;
		assert(!first.alignmentFailed);
		assert(!second.alignmentFailed);
		AlignmentResult finalResult;
		finalResult.alignmentFailed = false;
		finalResult.cellsProcessed = first.cellsProcessed + second.cellsProcessed;
		finalResult.elapsedMilliseconds = first.elapsedMilliseconds + second.elapsedMilliseconds;
		finalResult.alignment = first.alignment;
		finalResult.alignment.set_score(first.alignment.score() + second.alignment.score());
		int start = 0;
		auto firstEndPos = first.alignment.path().mapping(first.alignment.path().mapping_size()-1).position();;
		auto secondStartPos = second.alignment.path().mapping(0).position();
		if (posEqual(firstEndPos, secondStartPos))
		{
			start = 1;
		}
		else if (graph.outNeighbors[graph.nodeLookup.at(firstEndPos.node_id())].count(graph.nodeLookup.at(secondStartPos.node_id())) == 1)
		{
			start = 0;
		}
		else
		{
			std::cerr << "Piecewise alignments can't be merged!";
			std::cerr << " first end: " << firstEndPos.node_id() << " " << (firstEndPos.is_reverse() ? "-" : "+");
			std::cerr << " second start: " << secondStartPos.node_id() << " " << (secondStartPos.is_reverse() ? "-" : "+") << std::endl;
		}
		for (int i = start; i < second.alignment.path().mapping_size(); i++)
		{
			auto mapping = finalResult.alignment.mutable_path()->add_mapping();
			*mapping = second.alignment.path().mapping(i);
		}
		return finalResult;
	}

	AlignmentResult traceToAlignment(const std::string& seq_id, const std::string& sequence, ScoreType score, const std::vector<MatrixPosition>& trace, size_t cellsProcessed) const
	{
		vg::Alignment result;
		result.set_name(seq_id);
		result.set_score(score);
		result.set_sequence(sequence);
		auto path = new vg::Path;
		result.set_allocated_path(path);
		if (trace.size() == 0) return AlignmentResult { result, true, cellsProcessed, std::numeric_limits<size_t>::max() };
		size_t pos = 0;
		size_t oldNode = graph.indexToNode[trace[0].first];
		while (oldNode == graph.dummyNodeStart)
		{
			pos++;
			if (pos == trace.size()) return emptyAlignment(std::numeric_limits<size_t>::max(), cellsProcessed);
			assert(pos < trace.size());
			oldNode = graph.indexToNode[trace[pos].first];
			assert(oldNode < graph.nodeIDs.size());
		}
		if (oldNode == graph.dummyNodeEnd) return emptyAlignment(std::numeric_limits<size_t>::max(), cellsProcessed);
		int rank = 0;
		auto vgmapping = path->add_mapping();
		auto position = new vg::Position;
		vgmapping->set_allocated_position(position);
		vgmapping->set_rank(rank);
		position->set_node_id(graph.nodeIDs[oldNode]);
		position->set_is_reverse(graph.reverse[oldNode]);
		position->set_offset(trace[pos].first - graph.nodeStart[oldNode]);
		MatrixPosition btNodeStart = trace[pos];
		MatrixPosition btNodeEnd = trace[pos];
		for (; pos < trace.size(); pos++)
		{
			if (graph.indexToNode[trace[pos].first] == graph.dummyNodeEnd) break;
			if (graph.indexToNode[trace[pos].first] == oldNode)
			{
				btNodeEnd = trace[pos];
				continue;
			}
			assert(graph.indexToNode[btNodeEnd.first] == graph.indexToNode[btNodeStart.first]);
			assert(btNodeEnd.second >= btNodeStart.second);
			assert(btNodeEnd.first >= btNodeStart.first);
			auto edit = vgmapping->add_edit();
			edit->set_from_length(btNodeEnd.first - btNodeStart.first + 1);
			edit->set_to_length(btNodeEnd.second - btNodeStart.second + 1);
			edit->set_sequence(sequence.substr(btNodeStart.second, btNodeEnd.second - btNodeStart.second + 1));
			oldNode = graph.indexToNode[trace[pos].first];
			btNodeStart = trace[pos];
			btNodeEnd = trace[pos];
			rank++;
			vgmapping = path->add_mapping();
			position = new vg::Position;
			vgmapping->set_allocated_position(position);
			vgmapping->set_rank(rank);
			position->set_node_id(graph.nodeIDs[oldNode]);
			position->set_is_reverse(graph.reverse[oldNode]);
		}
		auto edit = vgmapping->add_edit();
		edit->set_from_length(btNodeEnd.first - btNodeStart.first);
		edit->set_to_length(btNodeEnd.second - btNodeStart.second);
		edit->set_sequence(sequence.substr(btNodeStart.second, btNodeEnd.second - btNodeStart.second));
		return AlignmentResult { result, false, cellsProcessed, std::numeric_limits<size_t>::max() };
	}

	class NodePosWithDistance
	{
	public:
		NodePosWithDistance(LengthType node, bool end, int distance) : node(node), end(end), distance(distance) {};
		LengthType node;
		bool end;
		int distance;
		bool operator<(const NodePosWithDistance& other) const
		{
			return distance < other.distance;
		}
		bool operator>(const NodePosWithDistance& other) const
		{
			return distance > other.distance;
		}
	};

	template <typename Container>
	void expandBandFromPositions(std::vector<bool>& band, const Container& startpositions, LengthType dynamicWidth, std::unordered_map<size_t, size_t>& distanceAtNodeStart, std::unordered_map<size_t, size_t>& distanceAtNodeEnd, std::set<size_t>* bandOrder, std::set<size_t>* bandOrderOutOfOrder) const
	{
		std::priority_queue<NodePosWithDistance, std::vector<NodePosWithDistance>, std::greater<NodePosWithDistance>> queue;
		for (auto startpos : startpositions)
		{
			auto nodeIndex = graph.indexToNode[startpos];
			band[nodeIndex] = true;
			if (nodeIndex < graph.firstInOrder && bandOrderOutOfOrder != nullptr)
			{
				bandOrderOutOfOrder->insert(nodeIndex);
			}
			else if (nodeIndex >= graph.firstInOrder && bandOrder != nullptr)
			{
				bandOrder->insert(nodeIndex);
			}
			auto start = graph.nodeStart[nodeIndex];
			auto end = graph.nodeEnd[nodeIndex];
			assert(end > startpos);
			assert(startpos >= start);
			assert(startpos - start >= 0);
			assert(end - startpos - 1 >= 0);
			queue.emplace(nodeIndex, false, startpos - start);
			queue.emplace(nodeIndex, true, end - startpos - 1);
		}
		int oldDistance = 0;
		while (queue.size() > 0)
		{
			NodePosWithDistance top = queue.top();
			assert(top.distance >= oldDistance);
			oldDistance = top.distance;
			assert(top.node < graph.nodeStart.size());
			queue.pop();
			assert(top.node < graph.nodeStart.size());
			if (top.distance > dynamicWidth) continue;
			if (top.end)
			{
				auto found = distanceAtNodeEnd.find(top.node);
				if (found != distanceAtNodeEnd.end() && found->second <= top.distance) continue;
				distanceAtNodeEnd[top.node] = top.distance;
			}
			else
			{
				auto found = distanceAtNodeStart.find(top.node);
				if (found != distanceAtNodeStart.end() && found->second <= top.distance) continue;
				distanceAtNodeStart[top.node] = top.distance;
			}
			auto nodeIndex = top.node;
			assert(nodeIndex < band.size());
			band[nodeIndex] = true;
			if (nodeIndex < graph.firstInOrder && bandOrderOutOfOrder != nullptr)
			{
				bandOrderOutOfOrder->insert(nodeIndex);
			}
			else if (nodeIndex >= graph.firstInOrder && bandOrder != nullptr)
			{
				bandOrder->insert(nodeIndex);
			}
			assert(nodeIndex < graph.nodeEnd.size());
			assert(nodeIndex < graph.nodeStart.size());
			auto size = graph.nodeEnd[nodeIndex] - graph.nodeStart[nodeIndex];
			if (top.end)
			{
				assert(top.distance + size - 1 >= top.distance);
				queue.emplace(nodeIndex, false, top.distance + size - 1);
				assert(nodeIndex < graph.outNeighbors.size());
				for (auto neighbor : graph.outNeighbors[nodeIndex])
				{
					assert(top.distance + 1 >= top.distance);
					queue.emplace(neighbor, false, top.distance + 1);
				}
			}
			else
			{
				assert(top.distance + size - 1 >= top.distance);
				queue.emplace(nodeIndex, true, top.distance + size - 1);
				assert(nodeIndex < graph.inNeighbors.size());
				for (auto neighbor : graph.inNeighbors[nodeIndex])
				{
					assert(top.distance + 1 >= top.distance);
					queue.emplace(neighbor, true, top.distance + 1);
				}
			}
		}
	}

	void projectForwardAndExpandBand(std::vector<bool>& band, LengthType previousMinimumIndex, LengthType dynamicWidth, std::set<size_t>* bandOrder, std::set<size_t>* bandOrderOutOfOrder) const
	{
		assert(previousMinimumIndex < graph.nodeSequences.size());
		auto nodeIndex = graph.indexToNode[previousMinimumIndex];
		auto end = graph.nodeEnd[nodeIndex];
		std::set<size_t> positions;
		positions.insert(previousMinimumIndex);
		positions = graph.ProjectForward(positions, WordConfiguration<Word>::WordSize);
		positions.insert(previousMinimumIndex);
		assert(positions.size() >= 1);
		band[nodeIndex] = true;
		if (nodeIndex < graph.firstInOrder && bandOrderOutOfOrder != nullptr)
		{
			bandOrderOutOfOrder->insert(nodeIndex);
		}
		else if (nodeIndex >= graph.firstInOrder && bandOrder != nullptr)
		{
			bandOrder->insert(nodeIndex);
		}
		std::unordered_map<size_t, size_t> distanceAtNodeEnd;
		std::unordered_map<size_t, size_t> distanceAtNodeStart;
		expandBandFromPositions(band, positions, dynamicWidth, distanceAtNodeStart, distanceAtNodeEnd, bandOrder, bandOrderOutOfOrder);
	}

	uint64_t bytePrefixSums(uint64_t value, int addition) const
	{
		value <<= WordConfiguration<Word>::ChunkBits;
		assert(addition >= 0);
		value += addition;
		return value * WordConfiguration<Word>::PrefixSumMultiplierConstant;
	}

	uint64_t byteVPVNSum(uint64_t prefixSumVP, uint64_t prefixSumVN) const
	{
		uint64_t result = WordConfiguration<Word>::SignMask;
		assert((prefixSumVP & result) == 0);
		assert((prefixSumVN & result) == 0);
		result += prefixSumVP;
		result -= prefixSumVN;
		result ^= WordConfiguration<Word>::SignMask;
		return result;
	}

#ifdef EXTRAASSERTIONS

	WordSlice getWordSliceCellByCell(size_t j, size_t w, const std::string& sequence, const NodeSlice<WordSlice>& currentSlice, const NodeSlice<WordSlice>& previousSlice, const std::vector<bool>& currentBand, const std::vector<bool>& previousBand) const
	{
		const auto lastBitMask = ((Word)1) << (WordConfiguration<Word>::WordSize-1);
		WordSlice result;
		auto nodeIndex = graph.indexToNode[w];
		assert(currentBand[nodeIndex]);
		const std::vector<WordSlice>& oldNode = previousBand[nodeIndex] ? previousSlice.node(nodeIndex) : currentSlice.node(nodeIndex);
		assert(currentBand[nodeIndex]);
		ScoreType current[66];
		current[0] = j+1;
		current[1] = j;
		if (j > 0 && previousBand[nodeIndex]) current[1] = std::min(current[1], oldNode[w-graph.nodeStart[nodeIndex]].scoreEnd);
		if (j > 0 && previousBand[nodeIndex]) current[0] = std::min(current[0], oldNode[w-graph.nodeStart[nodeIndex]].scoreEnd - ((oldNode[w-graph.nodeStart[nodeIndex]].VP & lastBitMask) ? 1 : 0) + ((oldNode[w-graph.nodeStart[nodeIndex]].VN & lastBitMask) ? 1 : 0));
		for (int i = 1; i < 65; i++)
		{
			current[i+1] = current[i]+1;
		}
		if (w == graph.nodeStart[nodeIndex])
		{
			for (auto neighbor : graph.inNeighbors[nodeIndex])
			{
				if (!previousBand[neighbor] && !currentBand[neighbor]) continue;
				const std::vector<WordSlice>& neighborSlice = currentBand[neighbor] ? currentSlice.node(neighbor) : previousSlice.node(neighbor);
				const std::vector<WordSlice>& oldNeighborSlice = previousBand[neighbor] ? previousSlice.node(neighbor) : currentSlice.node(neighbor);
				auto u = graph.nodeEnd[neighbor]-1;
				ScoreType previous[66];
				previous[0] = j+1;
				previous[1] = j;
				if (j > 0 && previousBand[neighbor]) previous[1] = std::min(previous[1], oldNeighborSlice.back().scoreEnd);
				if (j > 0 && previousBand[neighbor]) previous[0] = std::min(previous[0], oldNeighborSlice.back().scoreEnd - ((oldNeighborSlice.back().VP & lastBitMask) ? 1 : 0) + ((oldNeighborSlice.back().VN & lastBitMask) ? 1 : 0));
				if (currentBand[neighbor]) previous[1] = std::min(previous[1], neighborSlice.back().scoreBeforeStart);
				for (int i = 1; i < 65; i++)
				{
					if (currentBand[neighbor])
					{
						previous[i+1] = previous[i];
						previous[i+1] += (neighborSlice.back().VP & (((Word)1) << (i-1)) ? 1 : 0);
						previous[i+1] -= (neighborSlice.back().VN & (((Word)1) << (i-1)) ? 1 : 0);
					}
					else
					{
						previous[i+1] = previous[i]+1;
					}
				}
				current[0] = std::min(current[0], previous[0]+1);
				for (int i = 0; i < 65; i++)
				{
					current[i+1] = std::min(current[i+1], previous[i+1]+1);
					current[i+1] = std::min(current[i+1], current[i]+1);
					if (j+i > 0 && (sequence[j+i-1] == graph.nodeSequences[w] || sequence[j+i-1] == 'N'))
					{
						current[i+1] = std::min(current[i+1], previous[i]);
					}
					else
					{
						current[i+1] = std::min(current[i+1], previous[i]+1);
					}
				}
			}
		}
		else
		{
			const std::vector<WordSlice>& slice = currentSlice.node(nodeIndex);
			const std::vector<WordSlice>& oldSlice = previousBand[nodeIndex] ? previousSlice.node(nodeIndex) : slice;
			auto u = w-1;
			ScoreType previous[66];
			previous[0] = slice[u-graph.nodeStart[nodeIndex]].scoreBeforeStart+1;
			previous[1] = slice[u-graph.nodeStart[nodeIndex]].scoreBeforeStart;
			if (previousBand[nodeIndex]) previous[0] = std::min(previous[0], oldSlice[u-graph.nodeStart[nodeIndex]].scoreEnd - ((oldSlice[u-graph.nodeStart[nodeIndex]].VP & lastBitMask) ? 1 : 0) + ((oldSlice[u-graph.nodeStart[nodeIndex]].VN & lastBitMask) ? 1 : 0));
			if (previousBand[nodeIndex]) previous[1] = std::min(previous[1], oldSlice[u-graph.nodeStart[nodeIndex]].scoreEnd);
			for (int i = 1; i < 65; i++)
			{
				previous[i+1] = previous[i];
				previous[i+1] += (slice[u-graph.nodeStart[nodeIndex]].VP & (((Word)1) << (i-1)) ? 1 : 0);
				previous[i+1] -= (slice[u-graph.nodeStart[nodeIndex]].VN & (((Word)1) << (i-1)) ? 1 : 0);
			}
			current[0] = std::min(current[0], previous[0]+1);
			for (int i = 0; i < 65; i++)
			{
				current[i+1] = std::min(current[i+1], current[i]+1);
				current[i+1] = std::min(current[i+1], previous[i+1]+1);
				if (j+i > 0 && (sequence[j+i-1] == graph.nodeSequences[w] || sequence[j+i-1] == 'N'))
				{
					current[i+1] = std::min(current[i+1], previous[i]);
				}
				else
				{
					current[i+1] = std::min(current[i+1], previous[i]+1);
				}
			}
		}
		for (int i = 1; i < 65; i++)
		{
			assert(current[i+1] >= debugLastRowMinScore);
			assert(current[i+1] >= current[i]-1);
			assert(current[i+1] <= current[i]+1);
			if (current[i+1] == current[i]+1) result.VP |= ((Word)1) << (i-1);
			if (current[i+1] == current[i]-1) result.VN |= ((Word)1) << (i-1);
		}
		result.scoreBeforeStart = current[1];
		result.scoreEnd = current[65];
		assert(result.scoreEnd == result.scoreBeforeStart + WordConfiguration<Word>::popcount(result.VP) - WordConfiguration<Word>::popcount(result.VN));
		return result;
	}

#endif

#ifdef EXTRAASSERTIONS
	std::pair<uint64_t, uint64_t> differenceMasksCellByCell(uint64_t leftVP, uint64_t leftVN, uint64_t rightVP, uint64_t rightVN, int scoreDifference) const
	{
		int leftscore = 0;
		int rightscore = scoreDifference;
		uint64_t leftSmaller = 0;
		uint64_t rightSmaller = 0;
		for (int i = 0; i < 64; i++)
		{
			leftscore += leftVP & 1;
			leftscore -= leftVN & 1;
			rightscore += rightVP & 1;
			rightscore -= rightVN & 1;
			leftVP >>= 1;
			leftVN >>= 1;
			rightVP >>= 1;
			rightVN >>= 1;
			if (leftscore < rightscore) leftSmaller |= ((Word)1) << i;
			if (rightscore < leftscore) rightSmaller |= ((Word)1) << i;
		}
		return std::make_pair(leftSmaller, rightSmaller);
	}
#endif

	std::pair<uint64_t, uint64_t> differenceMasks(uint64_t leftVP, uint64_t leftVN, uint64_t rightVP, uint64_t rightVN, int scoreDifference) const
	{
#ifdef EXTRAASSERTIONS
		auto correctValue = differenceMasksCellByCell(leftVP, leftVN, rightVP, rightVN, scoreDifference);
#endif
		assert(scoreDifference >= 0);
		const uint64_t signmask = WordConfiguration<Word>::SignMask;
		const uint64_t lsbmask = WordConfiguration<Word>::LSBMask;
		const int chunksize = WordConfiguration<Word>::ChunkBits;
		const uint64_t allones = WordConfiguration<Word>::AllOnes;
		const uint64_t allzeros = WordConfiguration<Word>::AllZeros;
		uint64_t VPcommon = ~(leftVP & rightVP);
		uint64_t VNcommon = ~(leftVN & rightVN);
		leftVP &= VPcommon;
		leftVN &= VNcommon;
		rightVP &= VPcommon;
		rightVN &= VNcommon;
		//left is lower everywhere
		if (scoreDifference > WordConfiguration<Word>::popcount(rightVN) + WordConfiguration<Word>::popcount(leftVP))
		{
			return std::make_pair(allones, allzeros);
		}
		if (scoreDifference == 128 && rightVN == allones && leftVP == allones)
		{
			return std::make_pair(allones ^ ((Word)1 << (WordConfiguration<Word>::WordSize-1)), allzeros);
		}
		else if (scoreDifference == 0 && rightVN == allones && leftVP == allones)
		{
			return std::make_pair(0, allones);
		}
		assert(scoreDifference >= 0);
		assert(scoreDifference < 128);
		uint64_t byteVPVNSumLeft = byteVPVNSum(bytePrefixSums(WordConfiguration<Word>::ChunkPopcounts(leftVP), 0), bytePrefixSums(WordConfiguration<Word>::ChunkPopcounts(leftVN), 0));
		uint64_t byteVPVNSumRight = byteVPVNSum(bytePrefixSums(WordConfiguration<Word>::ChunkPopcounts(rightVP), scoreDifference), bytePrefixSums(WordConfiguration<Word>::ChunkPopcounts(rightVN), 0));
		uint64_t difference = byteVPVNSumLeft;
		{
			//take the bytvpvnsumright and split it from positive/negative values into two vectors with positive values, one which needs to be added and the other deducted
			//smearmask is 1 where the number needs to be deducted, and 0 where it needs to be added
			//except sign bits which are all 0
			uint64_t smearmask = ((byteVPVNSumRight & signmask) >> (chunksize-1)) * ((((Word)1) << (chunksize-1))-1);
			assert((smearmask & signmask) == 0);
			uint64_t deductions = ~smearmask & byteVPVNSumRight & ~signmask;
			//byteVPVNSumRight is in one's complement so take the not-value + 1
			uint64_t additions = (smearmask & ~byteVPVNSumRight) + (smearmask & lsbmask);
			assert((deductions & signmask) == 0);
			uint64_t signsBefore = difference & signmask;
			//unset the sign bits so additions don't interfere with other chunks
			difference &= ~signmask;
			difference += additions;
			//the sign bit is 1 if the value went from <0 to >=0
			//so in that case we need to flip it
			difference ^= signsBefore;
			signsBefore = difference & signmask;
			//set the sign bits so that deductions don't interfere with other chunks
			difference |= signmask;
			difference -= deductions;
			//sign bit is 0 if the value went from >=0 to <0
			//so flip them to the correct values
			signsBefore ^= signmask & ~difference;
			difference &= ~signmask;
			difference |= signsBefore;
		}
		//difference now contains the prefix sum difference (left-right) at each chunk
		uint64_t resultLeftSmallerThanRight = 0;
		uint64_t resultRightSmallerThanLeft = 0;
		for (int bit = 0; bit < chunksize; bit++)
		{
			uint64_t signsBefore = difference & signmask;
			//unset the sign bits so additions don't interfere with other chunks
			difference &= ~signmask;
			difference += leftVP & lsbmask;
			difference += rightVN & lsbmask;
			//the sign bit is 1 if the value went from <0 to >=0
			//so in that case we need to flip it
			difference ^= signsBefore;
			signsBefore = difference & signmask;
			//set the sign bits so that deductions don't interfere with other chunks
			difference |= signmask;
			difference -= leftVN & lsbmask;
			difference -= rightVP & lsbmask;
			//sign bit is 0 if the value went from >=0 to <0
			//so flip them to the correct values
			signsBefore ^= signmask & ~difference;
			difference &= ~signmask;
			difference |= signsBefore;
			leftVN >>= 1;
			leftVP >>= 1;
			rightVN >>= 1;
			rightVP >>= 1;
			//difference now contains the prefix sums difference (left-right) at each byte at (bit)'th bit
			//left < right when the prefix sum difference is negative (sign bit is set)
			uint64_t negative = (difference & signmask);
			resultLeftSmallerThanRight |= negative >> (WordConfiguration<Word>::ChunkBits - 1 - bit);
			//Test equality to zero. If it's zero, substracting one will make the sign bit 0, otherwise 1
			uint64_t notEqualToZero = ((difference | signmask) - lsbmask) & signmask;
			//right > left when the prefix sum difference is positive (not zero and not negative)
			resultRightSmallerThanLeft |= (notEqualToZero & ~negative) >> (WordConfiguration<Word>::ChunkBits - 1 - bit);
		}
#ifdef EXTRAASSERTIONS
		assert(resultLeftSmallerThanRight == correctValue.first);
		assert(resultRightSmallerThanLeft == correctValue.second);
#endif
		return std::make_pair(resultLeftSmallerThanRight, resultRightSmallerThanLeft);
	}

	WordSlice mergeTwoSlices(WordSlice left, WordSlice right) const
	{
		//optimization: 11% time inclusive 9% exclusive. can this be improved?
		//O(log w), because prefix sums need log w chunks of log w bits
		static_assert(std::is_same<Word, uint64_t>::value);
#ifdef EXTRAASSERTIONS
		auto correctValue = mergeTwoSlicesCellByCell(left, right);
#endif
		if (left.scoreBeforeStart > right.scoreBeforeStart) std::swap(left, right);
		WordSlice result;
		assert((left.VP & left.VN) == WordConfiguration<Word>::AllZeros);
		assert((right.VP & right.VN) == WordConfiguration<Word>::AllZeros);
		auto masks = differenceMasks(left.VP, left.VN, right.VP, right.VN, right.scoreBeforeStart - left.scoreBeforeStart);
		auto leftSmaller = masks.first;
		auto rightSmaller = masks.second;
		assert((leftSmaller & rightSmaller) == 0);
		auto mask = (rightSmaller | ((leftSmaller | rightSmaller) - (rightSmaller << 1))) & ~leftSmaller;
		uint64_t leftReduction = leftSmaller & (rightSmaller << 1);
		uint64_t rightReduction = rightSmaller & (leftSmaller << 1);
		if ((rightSmaller & 1) && left.scoreBeforeStart < right.scoreBeforeStart)
		{
			rightReduction |= 1;
		}
		assert((leftReduction & right.VP) == leftReduction);
		assert((rightReduction & left.VP) == rightReduction);
		assert((leftReduction & left.VN) == leftReduction);
		assert((rightReduction & right.VN) == rightReduction);
		left.VN &= ~leftReduction;
		right.VN &= ~rightReduction;
		result.VN = (left.VN & ~mask) | (right.VN & mask);
		result.VP = (left.VP & ~mask) | (right.VP & mask);
		assert((result.VP & result.VN) == 0);
		result.scoreBeforeStart = std::min(left.scoreBeforeStart, right.scoreBeforeStart);
		result.scoreEnd = std::min(left.scoreEnd, right.scoreEnd);
		assert(result.scoreEnd == result.scoreBeforeStart + WordConfiguration<Word>::popcount(result.VP) - WordConfiguration<Word>::popcount(result.VN));
#ifdef EXTRAASSERTIONS
		assert(result.VP == correctValue.VP);
		assert(result.VN == correctValue.VN);
		assert(result.scoreBeforeStart == correctValue.scoreBeforeStart);
		assert(result.scoreEnd == correctValue.scoreEnd);
#endif
		return result;
	}

#ifdef EXTRAASSERTIONS
	WordSlice mergeTwoSlicesCellByCell(WordSlice left, WordSlice right) const
	{
		assert((left.VP & left.VN) == WordConfiguration<Word>::AllZeros);
		assert((right.VP & right.VN) == WordConfiguration<Word>::AllZeros);
		ScoreType leftScore = left.scoreBeforeStart;
		WordSlice merged;
		merged.scoreBeforeStart = std::min(left.scoreBeforeStart, right.scoreBeforeStart);
		merged.VP = WordConfiguration<Word>::AllZeros;
		merged.VN = WordConfiguration<Word>::AllZeros;
		ScoreType rightScore = right.scoreBeforeStart;
		ScoreType previousScore = merged.scoreBeforeStart;
		for (size_t j = 0; j < WordConfiguration<Word>::WordSize; j++)
		{
			Word mask = ((Word)1) << j;
			if (left.VP & mask) leftScore++;
			else if (left.VN & mask) leftScore--;
			if (right.VN & mask) rightScore--;
			else if (right.VP & mask) rightScore++;
			ScoreType betterScore = std::min(leftScore, rightScore);
			if (betterScore == previousScore+1) merged.VP |= mask;
			else if (betterScore == previousScore-1) merged.VN |= mask;
			assert((merged.VP & merged.VN) == WordConfiguration<Word>::AllZeros);
			assert(betterScore >= previousScore-1);
			assert(betterScore <= previousScore+1);
			previousScore = betterScore;
		}
		merged.scoreEnd = previousScore;
		assert((merged.VP & merged.VN) == WordConfiguration<Word>::AllZeros);
		assert(merged.scoreEnd <= left.scoreEnd);
		assert(merged.scoreEnd <= right.scoreEnd);
		assert(merged.scoreBeforeStart <= left.scoreBeforeStart);
		assert(merged.scoreBeforeStart <= right.scoreBeforeStart);
		return merged;
	}
#endif

	WordSlice getNodeStartSlice(Word Eq, size_t nodeIndex, const NodeSlice<WordSlice>& previousSlice, const NodeSlice<WordSlice>& currentSlice, const std::vector<bool>& currentBand, const std::vector<bool>& previousBand, bool previousEq) const
	{
		//todo optimization: 10% time inclusive 4% exclusive. can this be improved?
		WordSlice previous;
		WordSlice previousUp;
		bool foundOne = false;
		bool foundOneUp = false;
		for (auto neighbor : graph.inNeighbors[nodeIndex])
		{
			if (currentBand[neighbor] && previousBand[neighbor]) assertSliceCorrectness(currentSlice.node(neighbor).back(), previousSlice.node(neighbor).back(), previousBand[neighbor]);
			if (previousBand[neighbor])
			{
				if (!foundOneUp)
				{
					previousUp = previousSlice.node(neighbor).back();
					foundOneUp = true;
				}
				else
				{
					auto competitor = previousSlice.node(neighbor).back();
					previousUp = mergeTwoSlices(previousUp, competitor);
				}
			}
			if (previousBand[neighbor] && !currentBand[neighbor])
			{
				if (!foundOne)
				{
					previous = getSourceSliceFromScore(previousSlice.node(neighbor).back().scoreEnd);
					foundOne = true;
				}
				else
				{
					auto competitor = getSourceSliceFromScore(previousSlice.node(neighbor).back().scoreEnd);
					previous = mergeTwoSlices(previous, competitor);
				}
			}
			if (!currentBand[neighbor]) continue;
			if (!foundOne)
			{
				previous = currentSlice.node(neighbor).back();
				foundOne = true;
			}
			else
			{
				auto competitor = currentSlice.node(neighbor).back();
				previous = mergeTwoSlices(previous, competitor);
			}
		}
		assert(foundOne);
		assertSliceCorrectness(previous, previousUp, foundOneUp);
		auto result = getNextSlice(Eq, previous, foundOneUp, previousEq, previousUp);
		return result;
	}

	WordSlice getSourceSliceWithoutBefore(size_t row) const
	{
		return { WordConfiguration<Word>::AllOnes, WordConfiguration<Word>::AllZeros, row+WordConfiguration<Word>::WordSize, row };
	}

	WordSlice getSourceSliceFromScore(ScoreType previousScore) const
	{
		return { WordConfiguration<Word>::AllOnes, WordConfiguration<Word>::AllZeros, previousScore+WordConfiguration<Word>::WordSize, previousScore };
	}

	WordSlice getSourceSlice(size_t nodeIndex, const NodeSlice<WordSlice>& previousSlice) const
	{
		return getSourceSliceFromScore(previousSlice.node(nodeIndex)[0].scoreEnd);
	}

	bool isSource(size_t nodeIndex, const std::vector<bool>& currentBand, const std::vector<bool>& previousBand) const
	{
		for (auto neighbor : graph.inNeighbors[nodeIndex])
		{
			if (currentBand[neighbor]) return false;
			if (previousBand[neighbor]) return false;
		}
		return true;
	}

	Word getEq(Word BA, Word BT, Word BC, Word BG, LengthType w) const
	{
		switch(graph.nodeSequences[w])
		{
			case 'A':
			return BA;
			break;
			case 'T':
			return BT;
			break;
			case 'C':
			return BC;
			break;
			case 'G':
			return BG;
			break;
			case '-':
			assert(false);
			break;
			default:
			assert(false);
			break;
		}
		assert(false);
		return 0;
	}

	WordSlice getNextSlice(Word Eq, WordSlice slice, bool previousInsideBand, bool previousEq, WordSlice previous) const
	{
		//optimization: 13% of time. probably can't be improved easily.
		//http://www.gersteinlab.org/courses/452/09-spring/pdf/Myers.pdf
		//pages 405 and 408

		auto oldValue = slice.scoreBeforeStart;
		if (!previousInsideBand)
		{
			slice.scoreBeforeStart += 1;
		}
		else
		{
			const auto lastBitMask = ((Word)1) << (WordConfiguration<Word>::WordSize - 1);
			assert(slice.scoreBeforeStart <= previous.scoreEnd);
			slice.scoreBeforeStart = std::min(slice.scoreBeforeStart + 1, previous.scoreEnd - ((previous.VP & lastBitMask) ? 1 : 0) + ((previous.VN & lastBitMask) ? 1 : 0) + (previousEq ? 0 : 1));
		}
		auto hin = slice.scoreBeforeStart - oldValue;

		Word Xv = Eq | slice.VN;
		//between 7 and 8
		if (hin < 0) Eq |= 1;
		Word Xh = (((Eq & slice.VP) + slice.VP) ^ slice.VP) | Eq;
		Word Ph = slice.VN | ~(Xh | slice.VP);
		Word Mh = slice.VP & Xh;
		Word lastBitMask = (((Word)1) << (WordConfiguration<Word>::WordSize - 1));
		if (Ph & lastBitMask)
		{
			slice.scoreEnd += 1;
		}
		else if (Mh & lastBitMask)
		{
			slice.scoreEnd -= 1;
		}
		Ph <<= 1;
		Mh <<= 1;
		//between 16 and 17
		if (hin < 0) Mh |= 1; else if (hin > 0) Ph |= 1;
		slice.VP = Mh | ~(Xv | Ph);
		slice.VN = Ph & Xv;

#ifndef NDEBUG
		auto wcvp = WordConfiguration<Word>::popcount(slice.VP);
		auto wcvn = WordConfiguration<Word>::popcount(slice.VN);
		assert(slice.scoreEnd == slice.scoreBeforeStart + wcvp - wcvn);
		assert(slice.scoreBeforeStart >= debugLastRowMinScore);
		assert(slice.scoreEnd >= debugLastRowMinScore);
#endif

		return slice;
	}

	class NodeCalculationResult
	{
	public:
		ScoreType minScore;
		LengthType minScoreIndex;
		size_t cellsProcessed;
	};

	bool firstZeroForced(const std::vector<bool>& previousBand, const std::vector<bool>& currentBand, LengthType neighborNodeIndex, WordSlice neighborSlice, Word currentEq) const
	{
		if (previousBand[neighborNodeIndex] && currentBand[neighborNodeIndex])
		{
			if (neighborSlice.VN & 1)
			{
				return true;
			}
			if (!(neighborSlice.VP & 1) && !(neighborSlice.VN & 1) && !(currentEq & 1))
			{
				return true;
			}
			return false;
		}
		else if (previousBand[neighborNodeIndex] && !currentBand[neighborNodeIndex])
		{
			return false;
		}
		else
		{
			return true;
		}
		assert(false);
	}

	void assertSliceCorrectness(const WordSlice& current, const WordSlice& up, bool previousBand) const
	{
#ifndef NDEBUG
			auto wcvp = WordConfiguration<Word>::popcount(current.VP);
			auto wcvn = WordConfiguration<Word>::popcount(current.VN);
			assert(current.scoreEnd == current.scoreBeforeStart + wcvp - wcvn);

			assert(current.scoreBeforeStart >= 0);
			assert(current.scoreEnd >= 0);
			assert(current.scoreBeforeStart <= current.scoreEnd + WordConfiguration<Word>::WordSize);
			assert(current.scoreEnd <= current.scoreBeforeStart + WordConfiguration<Word>::WordSize);
			assert((current.VP & current.VN) == WordConfiguration<Word>::AllZeros);

			assert(!previousBand || current.scoreBeforeStart <= up.scoreEnd);
			assert(current.scoreBeforeStart >= 0);
			assert(current.scoreEnd >= debugLastRowMinScore);
			assert(current.scoreBeforeStart >= debugLastRowMinScore);
#endif
	}

	NodeCalculationResult calculateNode(size_t i, size_t j, const std::string& sequence, Word BA, Word BT, Word BC, Word BG, NodeSlice<WordSlice>& currentSlice, const NodeSlice<WordSlice>& previousSlice, const std::vector<bool>& currentBand, const std::vector<bool>& previousBand, bool forceSource) const
	{
		//todo optimization: 42% inclusive 15% exclusive. can this be improved?
		NodeCalculationResult result;
		result.minScore = std::numeric_limits<ScoreType>::max();
		result.minScoreIndex = 0;
		result.cellsProcessed = 0;
		std::vector<WordSlice>& slice = currentSlice.node(i);
		const std::vector<WordSlice>& oldSlice = previousBand[i] ? previousSlice.node(i) : slice;
		assert(slice.size() == graph.nodeEnd[i] - graph.nodeStart[i]);
		auto nodeStart = graph.nodeStart[i];

#ifdef EXTRAASSERTIONS
		WordSlice correctstart;
		if (!forceSource)
		{
			correctstart = getWordSliceCellByCell(j, graph.nodeStart[i], sequence, currentSlice, previousSlice, currentBand, previousBand);
		}
#endif

		if (forceSource || isSource(i, currentBand, previousBand))
		{
			if (previousBand[i])
			{
				slice[0] = getSourceSlice(i, previousSlice);
			}
			else
			{
				slice[0] = getSourceSliceWithoutBefore(j);
			}
			if (slice[0].scoreEnd < result.minScore)
			{
				result.minScore = slice[0].scoreEnd;
				result.minScoreIndex = nodeStart;
			}
			assertSliceCorrectness(slice[0], oldSlice[0], previousBand[i]);
		}
		else
		{
			Word Eq = getEq(BA, BT, BC, BG, nodeStart);
			slice[0] = getNodeStartSlice(Eq, i, previousSlice, currentSlice, currentBand, previousBand, j == 0 || graph.nodeSequences[graph.nodeStart[i]] == sequence[j-1]);
			if (previousBand[i] && slice[0].scoreBeforeStart > oldSlice[0].scoreEnd)
			{
				slice[0] = mergeTwoSlices(getSourceSliceFromScore(oldSlice[0].scoreEnd), slice[0]);
			}
			if (slice[0].scoreBeforeStart > j)
			{
				slice[0] = mergeTwoSlices(getSourceSliceWithoutBefore(j), slice[0]);
			}
			if (slice[0].scoreEnd < result.minScore)
			{
				result.minScore = slice[0].scoreEnd;
				result.minScoreIndex = nodeStart;
			}
			assertSliceCorrectness(slice[0], oldSlice[0], previousBand[i]);
			//note: currentSlice[start].score - optimalInNeighborEndScore IS NOT within {-1, 0, 1} always because of the band
		}

#ifdef EXTRAASSERTIONS
		if (!forceSource)
		{
			assert(slice[0].scoreBeforeStart == correctstart.scoreBeforeStart);
			assert(slice[0].scoreEnd == correctstart.scoreEnd);
			assert(slice[0].VP == correctstart.VP);
			assert(slice[0].VN == correctstart.VN);
		}
#endif

		for (LengthType w = 1; w < graph.nodeEnd[i] - graph.nodeStart[i]; w++)
		{
			Word Eq = getEq(BA, BT, BC, BG, nodeStart+w);
			bool forceFirstHorizontalPositive = firstZeroForced(previousBand, currentBand, i, slice[w-1], Eq);

			slice[w] = getNextSlice(Eq, slice[w-1], previousBand[i], j == 0 || graph.nodeSequences[nodeStart+w] == sequence[j-1], oldSlice[w-1]);

			if (previousBand[i] && slice[w].scoreBeforeStart > oldSlice[w].scoreEnd)
			{
				slice[w] = mergeTwoSlices(getSourceSliceFromScore(oldSlice[w].scoreEnd), slice[w]);
			}
			if (slice[w].scoreBeforeStart > j)
			{
				slice[w] = mergeTwoSlices(getSourceSliceWithoutBefore(j), slice[w]);
			}

			assert(previousBand[i] || slice[w].scoreBeforeStart == j || slice[w].scoreBeforeStart == slice[w-1].scoreBeforeStart + 1);
			assertSliceCorrectness(slice[w], oldSlice[w], previousBand[i]);

			if (slice[w].scoreEnd <= result.minScore)
			{
				result.minScore = slice[w].scoreEnd;
				result.minScoreIndex = nodeStart + w;
			}

#ifdef EXTRAASSERTIONS
			if (!forceSource)
			{
				auto correctslice = getWordSliceCellByCell(j, nodeStart+w, sequence, currentSlice, previousSlice, currentBand, previousBand);
				assert(slice[w].scoreBeforeStart == correctslice.scoreBeforeStart);
				assert(slice[w].scoreEnd == correctslice.scoreEnd);
				assert(slice[w].VP == correctslice.VP);
				assert(slice[w].VN == correctslice.VN);
			}
#endif
		}
		result.cellsProcessed = (graph.nodeEnd[i] - graph.nodeStart[i]) * WordConfiguration<Word>::WordSize;
		return result;
	}

	void getCycleCutReachability(size_t j, size_t cycleCut, size_t index, const std::vector<bool>& currentBand, const std::vector<bool>& previousBand, std::vector<bool>& reachable, std::vector<bool>& source) const
	{
		assert(index < reachable.size());
		if (reachable[index]) return;
		reachable[index] = true;
		// assert(graph.notInOrder[cycleCut]);
		assert(currentBand[graph.cuts[cycleCut].nodes[index]]);
		if (graph.cuts[cycleCut].previousCut[index]) return;
		source[index] = true;
		for (auto otherIndex : graph.cuts[cycleCut].predecessors[index])
		{
			assert(otherIndex > index);
			if (previousBand[graph.cuts[cycleCut].nodes[otherIndex]])
			{
				source[index] = false;
			}
			if (currentBand[graph.cuts[cycleCut].nodes[otherIndex]])
			{
				getCycleCutReachability(j, cycleCut, otherIndex, currentBand, previousBand, reachable, source);
				source[index] = false;
			}
		}
	}

	void cutCycles(size_t j, const std::string& sequence, Word BA, Word BT, Word BC, Word BG, NodeSlice<WordSlice>& currentSlice, const NodeSlice<WordSlice>& previousSlice, const std::vector<bool>& currentBand, const std::vector<bool>& previousBand, const std::set<size_t>& bandOrderOutOfOrder) const
	{
		if (graph.firstInOrder == 0) return;
		for (auto& pair : currentSlice)
		{
			if (previousBand[pair.first])
			{
				pair.second.back() = getSourceSliceFromScore(previousSlice.node(pair.first).back().scoreEnd);
			}
			else
			{
				pair.second.back() = getSourceSliceWithoutBefore(j);
			}
		}
		//if there are cycles within 2*w of eachothers, calculating a latter slice may overwrite the earlier slice's value
		//store the correct values here and then merge them at the end
		std::unordered_map<size_t, WordSlice> correctEndValues;
		for (auto order : bandOrderOutOfOrder)
		{
			correctEndValues[order] = {WordConfiguration<Word>::AllZeros, WordConfiguration<Word>::AllZeros, std::numeric_limits<ScoreType>::max(), std::numeric_limits<ScoreType>::max()};
		}
		for (auto i : bandOrderOutOfOrder)
		{
			if (i == 0) continue;
			assert(currentBand[i]);
			assert(i > 0);
			assert(i < graph.firstInOrder);
			// assert(graph.notInOrder[i]);
			assert(graph.cuts[i].nodes.size() > 0);
			assert(graph.cuts[i].nodes[0] == i);
			std::vector<bool> reachable;
			std::vector<bool> source;
			reachable.resize(graph.cuts[i].nodes.size(), false);
			source.resize(graph.cuts[i].nodes.size(), false);
			getCycleCutReachability(j, i, 0, currentBand, previousBand, reachable, source);
			for (size_t index = graph.cuts[i].nodes.size()-1; index < graph.cuts[i].nodes.size(); index--)
			{
				if (!reachable[index]) continue;
				if (graph.cuts[i].previousCut[index])
				{
					assert(correctEndValues.find(graph.cuts[i].nodes[index]) != correctEndValues.end());
					assert(correctEndValues[graph.cuts[i].nodes[index]].scoreBeforeStart != std::numeric_limits<ScoreType>::max());
					currentSlice.node(graph.cuts[i].nodes[index]).back() = correctEndValues[graph.cuts[i].nodes[index]];
					if (previousBand[graph.cuts[i].nodes[index]]) assertSliceCorrectness(currentSlice.node(graph.cuts[i].nodes[index]).back(), previousSlice.node(graph.cuts[i].nodes[index]).back(), previousBand[graph.cuts[i].nodes[index]]);
				}
				else
				{
					calculateNode(graph.cuts[i].nodes[index], j, sequence, BA, BT, BC, BG, currentSlice, previousSlice, currentBand, previousBand, source[index]);
					if (previousBand[graph.cuts[i].nodes[index]]) assertSliceCorrectness(currentSlice.node(graph.cuts[i].nodes[index]).back(), previousSlice.node(graph.cuts[i].nodes[index]).back(), previousBand[graph.cuts[i].nodes[index]]);
				}
			}
			correctEndValues[i] = currentSlice.node(i).back();
			assert(graph.cuts[i].nodes[0] == i);
			for (size_t index = 1; index < graph.cuts[i].nodes.size(); index++)
			{
				auto node = graph.cuts[i].nodes[index];
				if (!currentBand[node]) continue;
				if (previousBand[node])
				{
					currentSlice.node(node).back() = getSourceSliceFromScore(previousSlice.node(node).back().scoreEnd);
				}
				else
				{
					currentSlice.node(node).back() = getSourceSliceWithoutBefore(j);
				}
			}
			currentSlice.node(i).back() = correctEndValues[i];
			if (previousBand[i]) assertSliceCorrectness(currentSlice.node(i).back(), previousSlice.node(i).back(), previousBand[i]);
		}
		for (auto i : bandOrderOutOfOrder)
		{
			if (i == 0) continue;
			assert(currentBand[i]);
			currentSlice.node(i).back() = correctEndValues[i];
			if (previousBand[i]) assertSliceCorrectness(currentSlice.node(i).back(), previousSlice.node(i).back(), previousBand[i]);
		}
	}

	void getBandOrder(const std::vector<bool>& currentBand, std::set<size_t>& bandOrder, std::set<size_t>& bandOrderOutOfOrder) const
	{
		assert(currentBand.size() == graph.notInOrder.size());
		for (size_t i = 0; i < graph.firstInOrder; i++)
		{
			if (currentBand[i]) bandOrderOutOfOrder.insert(i);
		}
		for (size_t i = graph.firstInOrder; i < currentBand.size(); i++)
		{
			if (currentBand[i]) bandOrder.insert(i);
		}
	}

	MatrixSlice getBitvectorSliceScoresAndFinalPosition(const std::string& sequence, int dynamicWidth, std::vector<std::vector<bool>>& startBand, LengthType dynamicRowStart, ScoreType maxScore) const
	{
		//todo optimization: 82% inclusive 17% exclusive. can this be improved?
		MatrixSlice result;
		result.cellsProcessed = 0;
		result.minScorePerWordSlice.emplace_back(0);
		result.minScoreIndexPerWordSlice.emplace_back(0);

		NodeSlice<WordSlice> previousSlice;

		LengthType previousMinimumIndex = std::numeric_limits<LengthType>::max();
		std::vector<bool> currentBand;
		std::vector<bool> previousBand;
		assert(startBand.size() > 0);
		assert(startBand[0].size() == graph.nodeStart.size());
		currentBand.resize(graph.nodeStart.size(), false);
		previousBand.resize(graph.nodeStart.size(), false);

		std::set<size_t> previousBandOrder;
		std::set<size_t> previousBandOrderOutOfOrder;

#ifndef NDEBUG
		debugLastRowMinScore = 0;
#endif

		for (size_t j = 0; j < sequence.size(); j += WordConfiguration<Word>::WordSize)
		{
			NodeSlice<WordSlice> currentSlice;
			ScoreType currentMinimumScore = std::numeric_limits<ScoreType>::max();
			LengthType currentMinimumIndex = std::numeric_limits<LengthType>::max();
			//preprocessed bitvectors for character equality
			Word BA = WordConfiguration<Word>::AllZeros;
			Word BT = WordConfiguration<Word>::AllZeros;
			Word BC = WordConfiguration<Word>::AllZeros;
			Word BG = WordConfiguration<Word>::AllZeros;
			for (int i = 0; i < WordConfiguration<Word>::WordSize && j+i < sequence.size(); i++)
			{
				Word mask = ((Word)1) << i;
				switch(sequence[j+i])
				{
					case 'A':
					case 'a':
					BA |= mask;
					break;
					case 'T':
					case 't':
					BT |= mask;
					break;
					case 'C':
					case 'c':
					BC |= mask;
					break;
					case 'G':
					case 'g':
					BG |= mask;
					break;
					case 'N':
					case 'n':
					BA |= mask;
					BC |= mask;
					BT |= mask;
					BG |= mask;
					break;
					case 'R':
					case 'r':
					BA |= mask;
					BG |= mask;
					break;
					case 'Y':
					case 'y':
					BC |= mask;
					BT |= mask;
					break;
					case 'K':
					case 'k':
					BG |= mask;
					BT |= mask;
					break;
					case 'M':
					case 'm':
					BA |= mask;
					BC |= mask;
					break;
					case 'S':
					case 's':
					BC |= mask;
					BG |= mask;
					break;
					case 'W':
					case 'w':
					BA |= mask;
					BT |= mask;
					break;
					case 'B':
					case 'b':
					BC |= mask;
					BG |= mask;
					BT |= mask;
					break;
					case 'D':
					case 'd':
					BA |= mask;
					BG |= mask;
					BT |= mask;
					break;
					case 'H':
					case 'h':
					BA |= mask;
					BC |= mask;
					BT |= mask;
					break;
					case 'V':
					case 'v':
					BA |= mask;
					BC |= mask;
					BG |= mask;
					break;
					default:
					assert(false);
					break;
				}
			}
			size_t slice = j / WordConfiguration<Word>::WordSize;
			std::set<size_t> bandOrder;
			std::set<size_t> bandOrderOutOfOrder;
			if (startBand.size() > slice)
			{
				if (slice > 0) previousBand = std::move(currentBand);
				currentBand = startBand[slice];
				getBandOrder(currentBand, bandOrder, bandOrderOutOfOrder);
				if (slice == 0)
				{
					previousBand = currentBand;
					previousBandOrder = bandOrder;
					previousBandOrderOutOfOrder = bandOrderOutOfOrder;
					for (auto node : previousBandOrder)
					{
						previousSlice.addNode(node, graph.nodeEnd[node]-graph.nodeStart[node]);
						std::vector<WordSlice>& slice = previousSlice.node(node);
						for (size_t i = 0; i < graph.nodeEnd[node] - graph.nodeStart[node]; i++)
						{
							slice[i] = {0, 0, 0, 0};
						}
					}
					for (auto node : previousBandOrderOutOfOrder)
					{
						previousSlice.addNode(node, graph.nodeEnd[node]-graph.nodeStart[node]);
						std::vector<WordSlice>& slice = previousSlice.node(node);
						for (size_t i = 0; i < graph.nodeEnd[node] - graph.nodeStart[node]; i++)
						{
							slice[i] = {0, 0, 0, 0};
						}
					}
				}
			}
			else
			{
				std::swap(currentBand, previousBand);
				assert(previousMinimumIndex != std::numeric_limits<LengthType>::max());
				projectForwardAndExpandBand(currentBand, previousMinimumIndex, dynamicWidth, &bandOrder, &bandOrderOutOfOrder);
			}
			for (auto i : bandOrder)
			{
				currentSlice.addNode(i, graph.nodeEnd[i] - graph.nodeStart[i]);
			}
			for (auto i : bandOrderOutOfOrder)
			{
				currentSlice.addNode(i, graph.nodeEnd[i] - graph.nodeStart[i]);
			}
			assert(bandOrder.size() > 0 || bandOrderOutOfOrder.size() > 0);
			cutCycles(j, sequence, BA, BT, BC, BG, currentSlice, previousSlice, currentBand, previousBand, bandOrderOutOfOrder);
			for (auto i : bandOrder)
			{
				assert(currentBand[i]);
				auto nodeCalc = calculateNode(i, j, sequence, BA, BT, BC, BG, currentSlice, previousSlice, currentBand, previousBand, false);
				assert(result.minScorePerWordSlice.size() == 0 || nodeCalc.minScore >= result.minScorePerWordSlice.back());
				if (nodeCalc.minScore < currentMinimumScore)
				{
					currentMinimumScore = nodeCalc.minScore;
					currentMinimumIndex = nodeCalc.minScoreIndex;
				}
				if (nodeCalc.minScore <= currentMinimumScore)
				{
					if (nodeCalc.minScoreIndex == graph.nodeEnd[i]-1)
					{
						if (currentSlice.node(i).back().VP & ((Word)1 << (WordConfiguration<Word>::WordSize - 1)))
						{
							for (auto neighbor : graph.outNeighbors[i])
							{
								if (sequence[j + WordConfiguration<Word>::WordSize - 1] == graph.nodeSequences[graph.nodeStart[neighbor]])
								{
									assert(nodeCalc.minScore > 0);
									currentMinimumScore = nodeCalc.minScore - 1;
									currentMinimumIndex == graph.nodeStart[neighbor];
								}
							}
						}
					}
				}
				result.cellsProcessed += nodeCalc.cellsProcessed;
			}
			for (auto i : bandOrderOutOfOrder)
			{
				assert(currentBand[i]);
				auto nodeCalc = calculateNode(i, j, sequence, BA, BT, BC, BG, currentSlice, previousSlice, currentBand, previousBand, false);
				assert(result.minScorePerWordSlice.size() == 0 || nodeCalc.minScore >= result.minScorePerWordSlice.back());
				if (nodeCalc.minScore < currentMinimumScore)
				{
					currentMinimumScore = nodeCalc.minScore;
					currentMinimumIndex = nodeCalc.minScoreIndex;
				}
				if (nodeCalc.minScore <= currentMinimumScore)
				{
					if (nodeCalc.minScoreIndex == graph.nodeEnd[i]-1)
					{
						if (currentSlice.node(i).back().VP & ((Word)1 << (WordConfiguration<Word>::WordSize - 1)))
						{
							for (auto neighbor : graph.outNeighbors[i])
							{
								if (sequence[j + WordConfiguration<Word>::WordSize - 1] == graph.nodeSequences[graph.nodeStart[neighbor]])
								{
									assert(nodeCalc.minScore > 0);
									currentMinimumScore = nodeCalc.minScore - 1;
									currentMinimumIndex == graph.nodeStart[neighbor];
								}
							}
						}
					}
				}
				result.cellsProcessed += nodeCalc.cellsProcessed;
			}
			for (auto node : previousBandOrder)
			{
				assert(previousBand[node]);
				previousBand[node] = false;
			}
			for (auto node : previousBandOrderOutOfOrder)
			{
				assert(previousBand[node]);
				previousBand[node] = false;
			}
			assert(currentMinimumIndex != std::numeric_limits<LengthType>::max());
			assert(result.minScorePerWordSlice.size() == 0 || currentMinimumScore >= result.minScorePerWordSlice.back());
			previousSlice = std::move(currentSlice);
			previousMinimumIndex = currentMinimumIndex;
			result.minScorePerWordSlice.emplace_back(currentMinimumScore);
			result.minScoreIndexPerWordSlice.emplace_back(currentMinimumIndex);
			previousBandOrder = std::move(bandOrder);
			previousBandOrderOutOfOrder = std::move(bandOrderOutOfOrder);
#ifndef NDEBUG
			debugLastRowMinScore = currentMinimumScore;
#endif
			if (currentMinimumScore > maxScore)
			{
				for (int i = j + WordConfiguration<Word>::WordSize; i < sequence.size(); i += WordConfiguration<Word>::WordSize)
				{
					result.minScorePerWordSlice.push_back(sequence.size());
					result.minScoreIndexPerWordSlice.push_back(0);
				}
				break;
			}
		}
#ifndef NDEBUG
		for (size_t i = 1; i < result.minScorePerWordSlice.size(); i++)
		{
			assert(result.minScorePerWordSlice[i] >= result.minScorePerWordSlice[i-1]);
		}
#endif
		return result;
	}

	std::vector<std::vector<bool>> getExtendedNodeBand(LengthType nodeIndex, LengthType startExtensionWidth) const
	{
		std::vector<std::vector<bool>> result;
		result.resize(1);
		result[0].resize(graph.nodeStart.size(), false);
		std::set<size_t> visited;
		std::priority_queue<NodePosWithDistance, std::vector<NodePosWithDistance>, std::greater<NodePosWithDistance>> queue;
		queue.emplace(nodeIndex, true, 0);
		while (queue.size() > 0)
		{
			auto top = queue.top();
			queue.pop();
			if (top.distance > startExtensionWidth) continue;
			if (visited.count(top.node) == 1) continue;
			result[0][top.node] = true;
			visited.insert(top.node);
			auto newDistance = top.distance + graph.nodeEnd[top.node] - graph.nodeStart[top.node];
			for (auto neighbor : graph.outNeighbors[top.node])
			{
				queue.emplace(neighbor, true, newDistance);
			}
		}
		return result;
	}

	TwoDirectionalSplitAlignment getSplitAlignment(const std::string& sequence, int dynamicWidth, int startExtensionWidth, LengthType matchBigraphNodeId, LengthType matchSequencePosition, ScoreType maxScore) const
	{
		assert(matchSequencePosition > 0);
		assert(matchSequencePosition < sequence.size() - 1);
		auto backwardPart = CommonUtils::ReverseComplement(sequence.substr(0, matchSequencePosition));
		auto forwardPart = sequence.substr(matchSequencePosition);
		int backwardpadding = (WordConfiguration<Word>::WordSize - (backwardPart.size() % WordConfiguration<Word>::WordSize)) % WordConfiguration<Word>::WordSize;
		assert(backwardpadding < WordConfiguration<Word>::WordSize);
		for (int i = 0; i < backwardpadding; i++)
		{
			backwardPart += 'N';
		}
		int forwardpadding = (WordConfiguration<Word>::WordSize - (forwardPart.size() % WordConfiguration<Word>::WordSize)) % WordConfiguration<Word>::WordSize;
		assert(forwardpadding < WordConfiguration<Word>::WordSize);
		for (int i = 0; i < forwardpadding; i++)
		{
			forwardPart += 'N';
		}
		assert(backwardPart.size() + forwardPart.size() <= sequence.size() + 2 * WordConfiguration<Word>::WordSize);
		auto forwardNode = graph.nodeLookup.at(matchBigraphNodeId * 2);
		auto backwardNode = graph.nodeLookup.at(matchBigraphNodeId * 2 + 1);
		assert(graph.nodeSequences.substr(graph.nodeStart[forwardNode], graph.nodeEnd[forwardNode] - graph.nodeStart[forwardNode]) == CommonUtils::ReverseComplement(graph.nodeSequences.substr(graph.nodeStart[backwardNode], graph.nodeEnd[backwardNode] - graph.nodeStart[backwardNode])));
		assert(graph.nodeEnd[forwardNode] - graph.nodeStart[forwardNode] == graph.nodeEnd[backwardNode] - graph.nodeStart[backwardNode]);
		auto forwardBand = getExtendedNodeBand(forwardNode, startExtensionWidth);
		auto forwardSlice = getBitvectorSliceScoresAndFinalPosition(forwardPart, dynamicWidth, forwardBand, WordConfiguration<Word>::WordSize, maxScore);
		auto backwardBand = getExtendedNodeBand(backwardNode, startExtensionWidth);
		auto backwardSlice = getBitvectorSliceScoresAndFinalPosition(backwardPart, dynamicWidth, backwardBand, WordConfiguration<Word>::WordSize, maxScore);
		auto reverseForwardSlice = getBitvectorSliceScoresAndFinalPosition(forwardPart, dynamicWidth, backwardBand, WordConfiguration<Word>::WordSize, maxScore);
		auto reverseBackwardSlice = getBitvectorSliceScoresAndFinalPosition(backwardPart, dynamicWidth, forwardBand, WordConfiguration<Word>::WordSize, maxScore);
		auto firstscore = forwardSlice.minScorePerWordSlice.back() + backwardSlice.minScorePerWordSlice.back();
		auto secondscore = reverseForwardSlice.minScorePerWordSlice.back() + reverseBackwardSlice.minScorePerWordSlice.back();
		assert(firstscore <= backwardPart.size() + forwardPart.size());
		assert(secondscore <= backwardPart.size() + forwardPart.size());
		std::cerr << "first direction score: " << firstscore << std::endl;
		std::cerr << "other direction score: " << secondscore << std::endl;
		if (firstscore < secondscore)
		{
			TwoDirectionalSplitAlignment result;
			result.sequenceSplitIndex = matchSequencePosition;
			result.scoresForward = forwardSlice.minScorePerWordSlice;
			result.scoresBackward = backwardSlice.minScorePerWordSlice;
			result.minIndicesForward = forwardSlice.minScoreIndexPerWordSlice;
			result.minIndicesBackward = backwardSlice.minScoreIndexPerWordSlice;
			result.nodeSize = graph.nodeEnd[forwardNode] - graph.nodeStart[forwardNode];
			result.startExtensionWidth = startExtensionWidth;
			return result;
		}
		else
		{
			TwoDirectionalSplitAlignment result;
			result.sequenceSplitIndex = matchSequencePosition;
			result.scoresForward = reverseForwardSlice.minScorePerWordSlice;
			result.scoresBackward = reverseBackwardSlice.minScorePerWordSlice;
			result.minIndicesForward = reverseForwardSlice.minScoreIndexPerWordSlice;
			result.minIndicesBackward = reverseBackwardSlice.minScoreIndexPerWordSlice;
			result.nodeSize = graph.nodeEnd[forwardNode] - graph.nodeStart[forwardNode];
			result.startExtensionWidth = startExtensionWidth;
			return result;
		}
	}

	std::vector<ScoreType> getMergedSplitScores(const std::vector<ScoreType>& backward, const std::vector<ScoreType>& forward, size_t nodeSize, size_t startExtensionWidth) const
	{
		std::vector<ScoreType> partialScores;
		ScoreType endScore = 0;
		for (size_t i = backward.size()-1; i < backward.size(); i--)
		{
			endScore += backward[i];
			partialScores.push_back(endScore);
		}
		for (size_t i = 0; i < forward.size(); i++)
		{
			endScore += forward[i];
			partialScores.push_back(endScore);
		}
		partialScores.back() += nodeSize + startExtensionWidth * 2;
		return partialScores;
	}

	std::vector<MatrixPosition> reverseTrace(std::vector<MatrixPosition> trace) const
	{
		if (trace.size() == 0) return trace;
		std::reverse(trace.begin(), trace.end());
		auto secondMax = trace[0].second;
		for (size_t i = 0; i < trace.size(); i++)
		{
			trace[i].first = graph.GetReversePosition(trace[i].first);
			assert(trace[i].second <= secondMax);
			trace[i].second = secondMax - trace[i].second;
		}
		return trace;
	}

	template <typename T>
	T factorial(T n) const
	{
		T result {1};
		for (int i = 2; i <= n; i += 1)
		{
			result *= i;
		}
		return result;
	}

	template <typename T>
	T choose(T n, T k) const
	{
		return factorial<T>(n) / factorial<T>(k) / factorial<T>(n - k);
	}

	template <typename T>
	T powr(T base, int exponent) const
	{
		if (exponent == 0) return T{1};
		if (exponent == 1) return base;
		if (exponent % 2 == 0)
		{
			auto part = powr(base, exponent / 2);
			assert(part > 0);
			return part * part;
		}
		if (exponent % 2 == 1)
		{
			auto part = powr(base, exponent / 2);
			assert(part > 0);
			return part * part * base;
		}
		assert(false);
	}

	std::vector<bool> estimateCorrectAlignmentViterbi(const std::vector<ScoreType>& scores) const
	{
		const boost::rational<boost::multiprecision::cpp_int> correctMismatchProbability {15, 100}; //15% from pacbio error rate
		const boost::rational<boost::multiprecision::cpp_int> falseMismatchProbability {50, 100}; //50% empirically
		const boost::rational<boost::multiprecision::cpp_int> falseToCorrectTransitionProbability = {1, 100}; //1% arbitrarily
		const boost::rational<boost::multiprecision::cpp_int> correctToFalseTransitionProbability = {1, 100}; //1% arbitrarily
		boost::rational<boost::multiprecision::cpp_int> correctProbability {30, 100}; //30% arbitrarily
		boost::rational<boost::multiprecision::cpp_int> falseProbability {70, 100}; //70% arbitrarily
		std::vector<bool> falseFromCorrectBacktrace;
		std::vector<bool> correctFromCorrectBacktrace;
		for (size_t i = 1; i < scores.size(); i++)
		{
			assert(scores[i] >= scores[i-1]);
			auto scorediff = scores[i] - scores[i-1];
			correctFromCorrectBacktrace.push_back(correctProbability * (1 - correctToFalseTransitionProbability) >= falseProbability * falseToCorrectTransitionProbability);
			falseFromCorrectBacktrace.push_back(correctProbability * correctToFalseTransitionProbability >= falseProbability * (1 - falseToCorrectTransitionProbability));
			boost::rational<boost::multiprecision::cpp_int> newCorrectProbability = std::max(correctProbability * (1 - correctToFalseTransitionProbability), falseProbability * falseToCorrectTransitionProbability);
			boost::rational<boost::multiprecision::cpp_int> newFalseProbability = std::max(correctProbability * correctToFalseTransitionProbability, falseProbability * (1 - falseToCorrectTransitionProbability));
			auto chooseresult = choose<boost::multiprecision::cpp_int>(WordConfiguration<Word>::WordSize, scorediff);
			auto correctMultiplier = chooseresult * powr(correctMismatchProbability, scorediff) * powr(1 - correctMismatchProbability, WordConfiguration<Word>::WordSize - scorediff);
			auto falseMultiplier = chooseresult * powr(falseMismatchProbability, scorediff) * powr(1 - falseMismatchProbability, WordConfiguration<Word>::WordSize - scorediff);
			newCorrectProbability *= correctMultiplier;
			newFalseProbability *= falseMultiplier;
			correctProbability = newCorrectProbability;
			falseProbability = newFalseProbability;
			auto normalizer = correctProbability + falseProbability;
			correctProbability /= normalizer;
			falseProbability /= normalizer;
		}
		assert(falseFromCorrectBacktrace.size() == scores.size() - 1);
		assert(correctFromCorrectBacktrace.size() == scores.size() - 1);
		bool currentCorrect = correctProbability > falseProbability;
		std::vector<bool> result;
		result.resize(scores.size() - 1);
		for (size_t i = scores.size()-2; i < scores.size(); i--)
		{
			result[i] = currentCorrect;
			if (currentCorrect)
			{
				currentCorrect = correctFromCorrectBacktrace[i];
			}
			else
			{
				currentCorrect = falseFromCorrectBacktrace[i];
			}
		}
		return result;
	}

	std::pair<std::tuple<ScoreType, std::vector<MatrixPosition>>, std::tuple<ScoreType, std::vector<MatrixPosition>>> getPiecewiseTracesFromSplit(const TwoDirectionalSplitAlignment& split, const std::string& sequence) const
	{
		std::string backtraceSequence;
		std::string backwardBacktraceSequence;
		auto startpartsize = split.sequenceSplitIndex;
		auto endpartsize = sequence.size() - split.sequenceSplitIndex;
		int startpadding = (WordConfiguration<Word>::WordSize - (startpartsize % WordConfiguration<Word>::WordSize)) % WordConfiguration<Word>::WordSize;
		int endpadding = (WordConfiguration<Word>::WordSize - (endpartsize % WordConfiguration<Word>::WordSize)) % WordConfiguration<Word>::WordSize;
		backtraceSequence = sequence.substr(split.sequenceSplitIndex);
		backwardBacktraceSequence = CommonUtils::ReverseComplement(sequence.substr(0, split.sequenceSplitIndex));
		backtraceSequence.reserve(sequence.size() + endpadding);
		backwardBacktraceSequence.reserve(sequence.size() + startpadding);
		for (int i = 0; i < startpadding; i++)
		{
			backwardBacktraceSequence += 'N';
		}
		for (int i = 0; i < endpadding; i++)
		{
			backtraceSequence += 'N';
		}
		assert(backtraceSequence.size() % WordConfiguration<Word>::WordSize == 0);
		assert(backwardBacktraceSequence.size() % WordConfiguration<Word>::WordSize == 0);
		auto backtraceresult = estimateCorrectnessAndBacktraceBiggestPart(backtraceSequence, split.scoresForward, split.minIndicesForward);
		std::cerr << "fw score: " << std::get<0>(backtraceresult) << std::endl;
		auto reverseBacktraceResult = estimateCorrectnessAndBacktraceBiggestPart(backwardBacktraceSequence, split.scoresBackward, split.minIndicesBackward);
		std::cerr << "bw score: " << std::get<0>(reverseBacktraceResult) << std::endl;

		while (backtraceresult.second.size() > 0 && backtraceresult.second.back().second >= backtraceSequence.size() - endpadding)
		{
			assert(backtraceresult.second.back().second >= backtraceSequence.size() - endpadding);
			backtraceresult.second.pop_back();
		}
		while (reverseBacktraceResult.second.size() > 0 && reverseBacktraceResult.second.back().second >= backwardBacktraceSequence.size() - startpadding)
		{
			assert(reverseBacktraceResult.second.back().second >= backwardBacktraceSequence.size() - startpadding);
			reverseBacktraceResult.second.pop_back();
		}

		return std::make_pair(backtraceresult, reverseBacktraceResult);
	}

	std::tuple<ScoreType, std::vector<MatrixPosition>, size_t> getBacktrace(std::string sequence, int dynamicWidth, LengthType dynamicRowStart, std::vector<std::vector<bool>>& startBand) const
	{
		int padding = (WordConfiguration<Word>::WordSize - (sequence.size() % WordConfiguration<Word>::WordSize)) % WordConfiguration<Word>::WordSize;
		for (int i = 0; i < padding; i++)
		{
			sequence += 'N';
		}
		auto slice = getBitvectorSliceScoresAndFinalPosition(sequence, dynamicWidth, startBand, dynamicRowStart, sequence.size() * 0.4);
		std::cerr << "score: " << slice.minScorePerWordSlice.back() << std::endl;
		if (slice.minScorePerWordSlice.back() > sequence.size() * 0.4)
		{
			return std::make_tuple(std::numeric_limits<ScoreType>::max(), std::vector<MatrixPosition>{}, slice.cellsProcessed);
		}
		auto backtraceresult = estimateCorrectnessAndBacktraceBiggestPart(sequence, slice.minScorePerWordSlice, slice.minScoreIndexPerWordSlice);
		assert(backtraceresult.first <= slice.FinalMinScore());
		while (backtraceresult.second.back().second >= sequence.size() - padding)
		{
			backtraceresult.second.pop_back();
		}
		assert(backtraceresult.second[0].second == 0);
		assert(backtraceresult.second.back().second == sequence.size() - padding - 1);
		return std::make_tuple(slice.FinalMinScore(), backtraceresult.second, slice.cellsProcessed);
	}

	const AlignmentGraph& graph;
};

#endif