/*
    libmaus2
    Copyright (C) 2009-2014 German Tischler
    Copyright (C) 2011-2014 Genome Research Limited

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#if ! defined(LIBMAUS2_BAMBAM_PARALLEL_PARSEBLOCKWORKPACKAGEDISPATCHER_HPP)
#define LIBMAUS2_BAMBAM_PARALLEL_PARSEBLOCKWORKPACKAGEDISPATCHER_HPP

#include <libmaus2/bambam/parallel/DecompressedBlockAddPendingInterface.hpp>
#include <libmaus2/bambam/parallel/DecompressedBlockReturnInterface.hpp>
#include <libmaus2/bambam/parallel/ParsedBlockAddPendingInterface.hpp>
#include <libmaus2/bambam/parallel/ParsedBlockStallInterface.hpp>
#include <libmaus2/bambam/parallel/ParsePackageReturnInterface.hpp>
#include <libmaus2/parallel/SimpleThreadWorkPackageDispatcher.hpp>

namespace libmaus2
{
	namespace bambam
	{
		namespace parallel
		{
			// dispatcher for block parsing
			struct ParseBlockWorkPackageDispatcher : public libmaus2::parallel::SimpleThreadWorkPackageDispatcher
			{
				DecompressedBlockAddPendingInterface & addDecompressedPendingInterface;
				DecompressedBlockReturnInterface     & returnDecompressedInterface;
				ParsedBlockAddPendingInterface       & addParsedPendingInterface;
				ParsedBlockStallInterface            & parseStallInterface;
				ParsePackageReturnInterface          & packageReturnInterface;

				ParseBlockWorkPackageDispatcher(
					DecompressedBlockAddPendingInterface & raddDecompressedPendingInterface,
					DecompressedBlockReturnInterface     & rreturnDecompressedInterface,
					ParsedBlockAddPendingInterface       & raddParsedPendingInterface,
					ParsedBlockStallInterface            & rparseStallInterface,
					ParsePackageReturnInterface          & rpackageReturnInterface
				) : addDecompressedPendingInterface(raddDecompressedPendingInterface),
				    returnDecompressedInterface(rreturnDecompressedInterface),
				    addParsedPendingInterface(raddParsedPendingInterface),
				    parseStallInterface(rparseStallInterface),
				    packageReturnInterface(rpackageReturnInterface)
				{

				}

				virtual void dispatch(
					libmaus2::parallel::SimpleThreadWorkPackage * P,
					libmaus2::parallel::SimpleThreadPoolInterfaceEnqueTermInterface & /* tpi */
				)
				{
					ParseBlockWorkPackage * BP = dynamic_cast<ParseBlockWorkPackage *>(P);
					assert ( BP );

					// tpi.addLogStringWithThreadId("ParseBlockWorkPackageDispatcher::dispatch() block id " + libmaus2::util::NumberSerialisation::formatNumber(BP->decompressedblock->blockid,0));

					// can we parse all information in the decompressed input block?
					if ( BP->parseInfo->parseBlock(*(BP->decompressedblock),*(BP->parseBlock)) )
					{
						// tpi.addLogStringWithThreadId("ParseBlockWorkPackageDispatcher::dispatch() parseBlock true");

						// if this is the last input block
						if ( BP->decompressedblock->final )
						{
							// post process block (reorder pointers to original input order)
							BP->parseBlock->reorder();
							BP->parseBlock->final = true;
							BP->parseBlock->low   = BP->parseInfo->parseacc;
							BP->parseInfo->parseacc += BP->parseBlock->fill();
							addParsedPendingInterface.putParsedBlockAddPending(BP->parseBlock);
						}
						// otherwise parse block might not be full yet, stall it
						else
						{
							// std::cerr << "stalling on " << BP->decompressedblock->blockid << std::endl;
							parseStallInterface.putParsedBlockStall(BP->parseBlock);
						}

						// return decompressed input block (implies we are ready for the next one)
						returnDecompressedInterface.putDecompressedBlockReturn(BP->decompressedblock);
					}
					else
					{
						// tpi.addLogStringWithThreadId("ParseBlockWorkPackageDispatcher::dispatch() parseBlock false");

						// post process block (reorder pointers to original input order)
						BP->parseBlock->reorder();
						// put back last name
						BP->parseInfo->putBackLastName(*(BP->parseBlock));
						// not the last one
						BP->parseBlock->final = false;
						// set low
						BP->parseBlock->low   = BP->parseInfo->parseacc;
						// increase number of reads seen
						BP->parseInfo->parseacc += BP->parseBlock->fill();

						// parse block is full, add it to pending list
						addParsedPendingInterface.putParsedBlockAddPending(BP->parseBlock);
						// decompressed input block still has more data, mark it as pending again
						addDecompressedPendingInterface.putDecompressedBlockAddPending(BP->decompressedblock);
					}

					// return the work package
					packageReturnInterface.putReturnParsePackage(BP);
				}
			};
		}
	}
}
#endif
