/*
    libmaus
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
#include <libmaus/bambam/BamAlignment.hpp>
#include <libmaus/bambam/BamAlignmentPosComparator.hpp>
#include <libmaus/bambam/BamHeader.hpp>
#include <libmaus/lz/BgzfInflateBase.hpp>
#include <libmaus/lz/SnappyCompressorObjectFactory.hpp>
#include <libmaus/lz/ZlibCompressorObjectFactory.hpp>
#include <libmaus/lz/SimpleCompressedOutputStream.hpp>
#include <libmaus/parallel/LockedBool.hpp>
#include <libmaus/parallel/LockedQueue.hpp>
#include <libmaus/parallel/SimpleThreadPool.hpp>
#include <libmaus/parallel/SimpleThreadPoolWorkPackageFreeList.hpp>
#include <libmaus/sorting/ParallelStableSort.hpp>
#include <libmaus/util/GetObject.hpp>
#include <libmaus/util/TempFileRemovalContainer.hpp>

struct BamThreadPoolDecodeBamParseQueueInfo
{
	uint64_t packageid;
	std::pair<uint64_t,uint64_t> blockmeta;
	uint64_t baseid;
	uint64_t blockid;
	
	BamThreadPoolDecodeBamParseQueueInfo()
	: packageid(0), blockmeta(0,0), baseid(0), blockid(0)
	{
	
	}
	BamThreadPoolDecodeBamParseQueueInfo(
		uint64_t rpackageid,
		std::pair<uint64_t,uint64_t> rblockmeta,
		uint64_t rbaseid,
		uint64_t rblockid
	)
	: packageid(rpackageid), blockmeta(rblockmeta), baseid(rbaseid), blockid(rblockid)
	{
	
	}
	
	bool operator<(BamThreadPoolDecodeBamParseQueueInfo const & o) const
	{
		return blockid > o.blockid;
	}
};

struct BamThreadPoolDecodeBamProcessQueueInfo
{
	uint64_t packageid;
	uint64_t baseid;
	uint64_t blockid;
	
	BamThreadPoolDecodeBamProcessQueueInfo()
	: packageid(0), baseid(0), blockid(0)
	{
	
	}
	BamThreadPoolDecodeBamProcessQueueInfo(
		uint64_t rpackageid,
		uint64_t rbaseid,
		uint64_t rblockid
	)
	: packageid(rpackageid), baseid(rbaseid), blockid(rblockid)
	{
	
	}
	
	bool operator<(BamThreadPoolDecodeBamProcessQueueInfo const & o) const
	{
		return blockid > o.blockid;
	}
};

struct BamProcessBuffer
{
	typedef BamProcessBuffer this_type;
	typedef libmaus::util::unique_ptr<this_type>::type unique_ptr_type;
	typedef libmaus::util::shared_ptr<this_type>::type shared_ptr_type;
	
	typedef uint64_t pointer_type;

	libmaus::autoarray::AutoArray<pointer_type> B8;
	uint64_t const bspace;

	uint8_t * const ca;
	uint8_t * cc;
	
	pointer_type * const pa;
	pointer_type * pc;
	
	int64_t bufferid;
	
	void reset()
	{
		cc = ca;
		pc = pa;
	}
	
	BamProcessBuffer(uint64_t const bytesize)
	: B8(
		(bytesize + sizeof(pointer_type) - 1)/sizeof(pointer_type),
		false
	  ),
	  bspace(sizeof(pointer_type) * B8.size()),
	  ca(reinterpret_cast<uint8_t *>(B8.begin())), cc(ca),
	  pa(B8.end()), pc(pa),
	  bufferid(-1)
	{
	}
	
	bool put(uint8_t const * p, uint64_t const s)
	{
		uint64_t const ptrmult = 2;
		uint64_t const spaceused_data = cc-ca;
		uint64_t const spaceused_ptr = (pa-pc)*sizeof(pointer_type)*ptrmult;
		uint64_t const spaceused_len = sizeof(uint32_t);
		uint64_t const spaceused = (spaceused_data+spaceused_ptr+spaceused_len);
		uint64_t const spaceav = bspace - spaceused;
		
		if ( spaceav >= s + sizeof(uint32_t) + ptrmult*sizeof(pointer_type) )
		{
			*(--pc) = cc-ca;
			for ( unsigned int i = 0; i < sizeof(uint32_t); ++i )
				*(cc++) = (s >> (i*8)) & 0xFF;
			std::copy(p,p+s,cc);
			cc += s;
			return true;
		}
		else
		{
			return false;
		}
	}
};


struct BamThreadPoolDecodeReadPackage;
struct BamThreadPoolDecompressReadPackage;
struct BamThreadPoolBamParsePackage;
struct BamThreadPoolBamProcessPackage;
struct BamThreadPoolBamSortPackage;
struct BamThreadPoolBamWritePackage;

struct BamThreadPoolDecodeContextBase;

struct BamThreadPoolDecodeContextBaseConstantsBase
{
	enum bamthreadpooldecodecontextbase_dispatcher_ids
	{
		bamthreadpooldecodecontextbase_dispatcher_id_read = 0,
		bamthreadpooldecodecontextbase_dispatcher_id_decompress = 1,
		bamthreadpooldecodecontextbase_dispatcher_id_bamparse = 2,
		bamthreadpooldecodecontextbase_dispatcher_id_bamprocess = 3,
		bamthreadpooldecodecontextbase_dispatcher_id_bamsort = 4,
		bamthreadpooldecodecontextbase_dispatcher_id_bamwrite = 5
	};
	
	static unsigned int const bamthreadpooldecodecontextbase_dispatcher_priority_read = 10;
	static unsigned int const bamthreadpooldecodecontextbase_dispatcher_priority_decompress = 10;
	static unsigned int const bamthreadpooldecodecontextbase_dispatcher_priority_bamparse = 10;
	static unsigned int const bamthreadpooldecodecontextbase_dispatcher_priority_bamprocess = 10;
	static unsigned int const bamthreadpooldecodecontextbase_dispatcher_priority_bamsort = 0;
	static unsigned int const bamthreadpooldecodecontextbase_dispatcher_priority_bamwrite = 0;
};

struct BamThreadPoolDecodeReadPackage : public ::libmaus::parallel::SimpleThreadWorkPackage
{
	typedef BamThreadPoolDecodeReadPackage this_type;
	typedef libmaus::util::unique_ptr<this_type>::type unique_ptr_type;
	typedef libmaus::util::shared_ptr<this_type>::type shared_ptr_type;

	BamThreadPoolDecodeContextBase * contextbase;

	BamThreadPoolDecodeReadPackage() : contextbase(0) {}
	BamThreadPoolDecodeReadPackage(
		uint64_t const rpackageid,
		BamThreadPoolDecodeContextBase * rcontextbase
	)
	: ::libmaus::parallel::SimpleThreadWorkPackage(
		BamThreadPoolDecodeContextBaseConstantsBase::bamthreadpooldecodecontextbase_dispatcher_priority_read,
		BamThreadPoolDecodeContextBaseConstantsBase::bamthreadpooldecodecontextbase_dispatcher_id_read,
		rpackageid
	), contextbase(rcontextbase)
	{
	
	}

	virtual char const * getPackageName() const
	{
		return "BamThreadPoolDecodeReadPackage";
	}
};

struct BamThreadPoolDecodeDecompressPackage : public ::libmaus::parallel::SimpleThreadWorkPackage
{
	typedef BamThreadPoolDecodeDecompressPackage this_type;
	typedef libmaus::util::unique_ptr<this_type>::type unique_ptr_type;
	typedef libmaus::util::shared_ptr<this_type>::type shared_ptr_type;

	BamThreadPoolDecodeContextBase * contextbase;

	std::pair<uint64_t,uint64_t> blockmeta; // block size compressed and uncompressed
	uint64_t baseid;
	uint64_t blockid;

	BamThreadPoolDecodeDecompressPackage() : contextbase(0) {}
	BamThreadPoolDecodeDecompressPackage(
		uint64_t const rpackageid,
		BamThreadPoolDecodeContextBase * rcontextbase,
		std::pair<uint64_t,uint64_t> rblockmeta,
		uint64_t rbaseid,
		uint64_t rblockid
		
	)
	: ::libmaus::parallel::SimpleThreadWorkPackage(
		BamThreadPoolDecodeContextBaseConstantsBase::bamthreadpooldecodecontextbase_dispatcher_priority_decompress,
		BamThreadPoolDecodeContextBaseConstantsBase::bamthreadpooldecodecontextbase_dispatcher_id_decompress,
		rpackageid
	), contextbase(rcontextbase), blockmeta(rblockmeta), baseid(rbaseid), blockid(rblockid)
	{
	
	}

	virtual char const * getPackageName() const
	{
		return "BamThreadPoolDecodeDecompressPackage";
	}
};


struct BamThreadPoolDecodeBamParsePackage : public ::libmaus::parallel::SimpleThreadWorkPackage
{
	typedef BamThreadPoolDecodeBamParsePackage this_type;
	typedef libmaus::util::unique_ptr<this_type>::type unique_ptr_type;
	typedef libmaus::util::shared_ptr<this_type>::type shared_ptr_type;

	BamThreadPoolDecodeContextBase * contextbase;

	std::pair<uint64_t,uint64_t> blockmeta; // block size compressed and uncompressed
	uint64_t baseid;
	uint64_t blockid;

	BamThreadPoolDecodeBamParsePackage() : contextbase(0) {}
	BamThreadPoolDecodeBamParsePackage(
		uint64_t const rpackageid,
		BamThreadPoolDecodeContextBase * rcontextbase,
		std::pair<uint64_t,uint64_t> rblockmeta,
		uint64_t rbaseid,
		uint64_t rblockid		
	)
	: ::libmaus::parallel::SimpleThreadWorkPackage(
		BamThreadPoolDecodeContextBaseConstantsBase::bamthreadpooldecodecontextbase_dispatcher_priority_bamparse,
		BamThreadPoolDecodeContextBaseConstantsBase::bamthreadpooldecodecontextbase_dispatcher_id_bamparse,
		rpackageid
	), contextbase(rcontextbase), blockmeta(rblockmeta), baseid(rbaseid), blockid(rblockid)
	{
	
	}
	virtual char const * getPackageName() const
	{
		return "BamThreadPoolDecodeBamParsePackage";
	}
};

struct BamThreadPoolDecodeBamProcessPackage : public ::libmaus::parallel::SimpleThreadWorkPackage
{
	typedef BamThreadPoolDecodeBamProcessPackage this_type;
	typedef libmaus::util::unique_ptr<this_type>::type unique_ptr_type;
	typedef libmaus::util::shared_ptr<this_type>::type shared_ptr_type;

	BamThreadPoolDecodeContextBase * contextbase;
	uint64_t baseid;
	uint64_t blockid;

	BamThreadPoolDecodeBamProcessPackage() : contextbase(0) {}
	BamThreadPoolDecodeBamProcessPackage(
		uint64_t const rpackageid,
		BamThreadPoolDecodeContextBase * rcontextbase,
		uint64_t rbaseid,
		uint64_t rblockid		
	)
	: ::libmaus::parallel::SimpleThreadWorkPackage(
		BamThreadPoolDecodeContextBaseConstantsBase::bamthreadpooldecodecontextbase_dispatcher_priority_bamprocess,
		BamThreadPoolDecodeContextBaseConstantsBase::bamthreadpooldecodecontextbase_dispatcher_id_bamprocess,
		rpackageid
	), contextbase(rcontextbase), baseid(rbaseid), blockid(rblockid)
	{
	
	}
	virtual char const * getPackageName() const
	{
		return "BamThreadPoolDecodeBamProcessPackage";
	}
};


struct BamSortInfo
{
	typedef BamSortInfo this_type;
	typedef libmaus::util::unique_ptr<this_type>::type unique_ptr_type;
	typedef libmaus::util::shared_ptr<this_type>::type shared_ptr_type;
	
	typedef libmaus::bambam::BamAlignmentPosComparator order_type;
	typedef libmaus::sorting::ParallelStableSort::ParallelSortControl<
		BamProcessBuffer::pointer_type *,libmaus::bambam::BamAlignmentPosComparator
	> sort_type;
	
	BamProcessBuffer * processBuffer;
	libmaus::bambam::BamAlignmentPosComparator BAPC;
	sort_type::unique_ptr_type sortControl;
	
	BamSortInfo(
		BamProcessBuffer * rprocessBuffer, uint64_t const rnumthreads
	) : processBuffer(rprocessBuffer), BAPC(processBuffer->ca),
	    sortControl(new sort_type(
	    	processBuffer->pc, // current
	    	processBuffer->pa, // end
	    	processBuffer->pc-(processBuffer->pa-processBuffer->pc),
	    	processBuffer->pc,
	    	BAPC,
	    	rnumthreads,
	    	true /* copy back */
	    ))
	{
		assert ( sortControl->context.ae-sortControl->context.aa == sortControl->context.be-sortControl->context.ba );

		#if 0
		for ( uint64_t * pc = processBuffer->pc; pc != processBuffer->pa; ++pc )
		{
			uint8_t const * cp = processBuffer->ca + *pc + 4;
			std::cerr << libmaus::bambam::BamAlignmentDecoderBase::getReadName(cp) << std::endl;
		}
		#endif
	}
};

struct BamThreadPoolDecodeBamSortPackage : public ::libmaus::parallel::SimpleThreadWorkPackage
{
	typedef BamThreadPoolDecodeBamSortPackage this_type;
	typedef libmaus::util::unique_ptr<this_type>::type unique_ptr_type;
	typedef libmaus::util::shared_ptr<this_type>::type shared_ptr_type;

	enum sort_package_type {
		sort_package_type_base,
		sort_package_type_merge_level,
		sort_package_type_merge_sublevel,
	};

	BamThreadPoolDecodeContextBase * contextbase;
	uint64_t baseid;
	uint64_t blockid;
	BamSortInfo::shared_ptr_type sortinfo;
	sort_package_type packagetype;	
	
	uint64_t sort_base_id;
	uint64_t sort_merge_id;
	uint64_t sort_submerge_id;

	BamThreadPoolDecodeBamSortPackage() : contextbase(0), packagetype(sort_package_type_base) {}
	BamThreadPoolDecodeBamSortPackage(
		uint64_t const rpackageid,
		BamThreadPoolDecodeContextBase * rcontextbase,
		uint64_t rbaseid,
		uint64_t rblockid,
		BamSortInfo::shared_ptr_type rsortinfo,
		sort_package_type rpackagetype,
		uint64_t rsort_base_id,
		uint64_t rsort_merge_id,
		uint64_t rsort_submerge_id
	)
	: ::libmaus::parallel::SimpleThreadWorkPackage(
		BamThreadPoolDecodeContextBaseConstantsBase::bamthreadpooldecodecontextbase_dispatcher_priority_bamsort,
		BamThreadPoolDecodeContextBaseConstantsBase::bamthreadpooldecodecontextbase_dispatcher_id_bamsort,
		rpackageid
	), contextbase(rcontextbase), baseid(rbaseid), blockid(rblockid), sortinfo(rsortinfo), packagetype(rpackagetype),
	   sort_base_id(rsort_base_id), sort_merge_id(rsort_merge_id), sort_submerge_id(rsort_submerge_id)
	{
		assert ( 
			packagetype == sort_package_type_base ||
			packagetype == sort_package_type_merge_level ||
			packagetype == sort_package_type_merge_sublevel
		);
	}
	virtual char const * getPackageName() const
	{
		return "BamThreadPoolDecodeBamSortPackage";
	}
};

struct BamBlockWriteInfo
{
	typedef BamBlockWriteInfo this_type;
	typedef libmaus::util::unique_ptr<this_type>::type unique_ptr_type;
	typedef libmaus::util::shared_ptr<this_type>::type shared_ptr_type;
	
	BamProcessBuffer * processBuffer;
	std::vector < std::pair<BamProcessBuffer::pointer_type const *, BamProcessBuffer::pointer_type const *> > packets;
	libmaus::parallel::SynchronousCounter<uint64_t> packetsWritten;
	
	BamBlockWriteInfo() : processBuffer(0), packetsWritten(0) {}
	BamBlockWriteInfo(
		BamProcessBuffer * rprocessBuffer,
		std::vector < std::pair<BamProcessBuffer::pointer_type const *, BamProcessBuffer::pointer_type const *> > const & rpackets
	) : processBuffer(rprocessBuffer), packets(rpackets), packetsWritten(0) {}
};

struct BamThreadPoolDecodeBamWritePackage : public ::libmaus::parallel::SimpleThreadWorkPackage
{
	typedef BamThreadPoolDecodeBamWritePackage this_type;
	typedef libmaus::util::unique_ptr<this_type>::type unique_ptr_type;
	typedef libmaus::util::shared_ptr<this_type>::type shared_ptr_type;

	BamThreadPoolDecodeContextBase * contextbase;
	uint64_t baseid;
	uint64_t blockid;
	BamBlockWriteInfo::shared_ptr_type writeinfo;
	uint64_t write_block_id;

	BamThreadPoolDecodeBamWritePackage() : contextbase(0) {}
	BamThreadPoolDecodeBamWritePackage(
		uint64_t const rpackageid,
		BamThreadPoolDecodeContextBase * rcontextbase,
		uint64_t rbaseid,
		uint64_t rblockid,
		BamBlockWriteInfo::shared_ptr_type rwriteinfo,
		uint64_t rwrite_block_id
	)
	: ::libmaus::parallel::SimpleThreadWorkPackage(
		BamThreadPoolDecodeContextBaseConstantsBase::bamthreadpooldecodecontextbase_dispatcher_priority_bamwrite,
		BamThreadPoolDecodeContextBaseConstantsBase::bamthreadpooldecodecontextbase_dispatcher_id_bamwrite,
		rpackageid
	), contextbase(rcontextbase), baseid(rbaseid), blockid(rblockid), writeinfo(rwriteinfo),
	   write_block_id(rwrite_block_id)
	{
	}
	virtual char const * getPackageName() const
	{
		return "BamThreadPoolDecodeBamWritePackage";
	}
	
	bool operator<(BamThreadPoolDecodeBamWritePackage const & o) const
	{
		return baseid > o.baseid;
	}
};

struct BamThreadPoolDecodeBamWritePackageHeapComparator
{
	bool operator()(BamThreadPoolDecodeBamWritePackage const * A, BamThreadPoolDecodeBamWritePackage const * B)
	{
		return A->baseid > B->baseid;
	}
};

struct BamThreadPoolDecodeContextBase : public BamThreadPoolDecodeContextBaseConstantsBase
{
	static uint64_t const alperpackalign = 1024;

	libmaus::parallel::PosixSpinLock cerrlock;

	libmaus::parallel::SimpleThreadPoolWorkPackageFreeList<BamThreadPoolDecodeReadPackage> readFreeList;
	libmaus::parallel::SimpleThreadPoolWorkPackageFreeList<BamThreadPoolDecodeDecompressPackage> decompressFreeList;
	libmaus::parallel::SimpleThreadPoolWorkPackageFreeList<BamThreadPoolDecodeBamParsePackage> bamParseFreeList;
	libmaus::parallel::SimpleThreadPoolWorkPackageFreeList<BamThreadPoolDecodeBamProcessPackage> bamProcessFreeList;
	libmaus::parallel::SimpleThreadPoolWorkPackageFreeList<BamThreadPoolDecodeBamSortPackage> bamSortFreeList;
	libmaus::parallel::SimpleThreadPoolWorkPackageFreeList<BamThreadPoolDecodeBamWritePackage> bamWriteFreeList;
	
	libmaus::parallel::PosixSpinLock inputLock;
	libmaus::timing::RealTimeClock inputRtc;
	std::istream & in;
	libmaus::parallel::SynchronousCounter<uint64_t> readCnt;
	libmaus::parallel::SynchronousCounter<uint64_t> readCompCnt;
	libmaus::parallel::LockedBool readComplete;
	libmaus::parallel::PosixSemaphore readSem;
	libmaus::autoarray::AutoArray< libmaus::lz::BgzfInflateBase::unique_ptr_type > inflateBases;
	libmaus::autoarray::AutoArray< char > inflateDecompressSpace;
	libmaus::parallel::SynchronousQueue<uint64_t> inflateBasesFreeList;
	libmaus::parallel::SynchronousCounter<uint64_t> nextInputBlockId;
	
	libmaus::parallel::PosixSpinLock decompressLock;
	uint64_t decompressCnt;
	libmaus::parallel::LockedBool decompressComplete;
	
	libmaus::parallel::PosixSpinLock bamParseQueueLock;
	std::priority_queue<BamThreadPoolDecodeBamParseQueueInfo> bamparseQueue;
	std::priority_queue<BamThreadPoolDecodeBamParseQueueInfo> bamparseStall;
	uint64_t bamParseNextBlock;

	libmaus::parallel::PosixSpinLock bamParseLock;
	uint64_t bamParseCnt;
	libmaus::parallel::LockedBool bamParseComplete;

	libmaus::parallel::LockedBool haveheader;
	::libmaus::bambam::BamHeader::BamHeaderParserState bamheaderparsestate;
	libmaus::bambam::BamHeader header;
	
	enum bam_parser_state_type {
		bam_parser_state_read_blocklength,
		bam_parser_state_read_blockdata
	};
	
	bam_parser_state_type bamParserState;
	unsigned int bamBlockSizeRead;
	uint32_t bamBlockSizeValue;
	uint32_t bamBlockDataRead;
	libmaus::bambam::BamAlignment bamGatherBuffer;
	
	libmaus::autoarray::AutoArray<BamProcessBuffer::unique_ptr_type> processBuffers;
	libmaus::parallel::LockedQueue<uint64_t> processBuffersFreeList;
	uint64_t nextProcessBufferIdIn;
	libmaus::parallel::PosixSpinLock nextProcessBufferIdOutLock;
	uint64_t nextProcessBufferIdOut;

	libmaus::parallel::LockedBool bamProcessComplete;

	libmaus::parallel::PosixSpinLock buffersSortedLock;
	uint64_t buffersSorted;
	libmaus::parallel::LockedBool bamSortComplete;

	std::vector<std::string> tmpfilenames;
	libmaus::autoarray::AutoArray<libmaus::aio::CheckedOutputStream::unique_ptr_type> tmpfiles;
	// libmaus::lz::SnappyCompressorObjectFactory snappyCompressorFactory;
	libmaus::lz::ZlibCompressorObjectFactory compressorFactory;
	libmaus::autoarray::AutoArray<libmaus::lz::SimpleCompressedOutputStream<std::ostream>::unique_ptr_type> compressedTmpFiles;
	libmaus::parallel::PosixSpinLock tmpfileblockslock;
	std::vector< 
		std::vector< 
			std::pair<
				std::pair<uint64_t,uint64_t>,
				std::pair<uint64_t,uint64_t>
			> 
		> 
	> tmpfileblocks;
	std::vector< std::vector<uint64_t> > tmpfileblockcnts;
	std::vector< uint64_t > tmpfileblockcntsums;
	
	libmaus::parallel::PosixSpinLock writesPendingLock;
	std::vector< uint64_t > writesNext;
	std::vector< std::priority_queue<BamThreadPoolDecodeBamWritePackage *, std::vector<BamThreadPoolDecodeBamWritePackage *>, BamThreadPoolDecodeBamWritePackageHeapComparator> > writesPending;

	libmaus::parallel::PosixSpinLock buffersWrittenLock;
	uint64_t buffersWritten;
	libmaus::parallel::LockedBool bamWriteComplete;

	libmaus::parallel::SimpleThreadPool & TP;
		
	BamThreadPoolDecodeContextBase(
		std::istream & rin,
		uint64_t const numInflateBases,
		uint64_t const numProcessBuffers,
		uint64_t const processBufferSize,
		std::string const & tmpfilenamebase,
		uint64_t const numthreads,
		libmaus::parallel::SimpleThreadPool & rTP
	)
	:
	  inputLock(),
	  in(rin),
	  readCnt(0),
	  readCompCnt(0),
	  readComplete(false), 
	  inflateBases(numInflateBases),
	  inflateDecompressSpace(numInflateBases * libmaus::lz::BgzfConstants::getBgzfMaxBlockSize()),
	  inflateBasesFreeList(),
	  nextInputBlockId(0),
	  decompressLock(),
	  decompressCnt(0),
	  decompressComplete(false),
	  bamParseQueueLock(),
	  bamparseQueue(),
	  bamparseStall(),
	  bamParseNextBlock(0),
	  bamParseLock(),
	  bamParseCnt(0),
	  bamParseComplete(false),
	  haveheader(false),
	  bamheaderparsestate(),
	  header(),
	  bamParserState(bam_parser_state_read_blocklength),
	  bamBlockSizeRead(0),
	  bamBlockSizeValue(0),
	  bamBlockDataRead(0),
	  processBuffers(numProcessBuffers),
	  processBuffersFreeList(),
	  nextProcessBufferIdIn(0),
	  nextProcessBufferIdOut(0),
	  bamProcessComplete(false),
	  buffersSorted(0),
	  bamSortComplete(false),
	  tmpfilenames(numthreads),
	  tmpfiles(numthreads),
	  // compressorFactory(Z_BEST_SPEED),
	  compressorFactory(-1),
	  compressedTmpFiles(numthreads),
	  writesNext(numthreads,0),
	  writesPending(numthreads),
	  buffersWritten(0),
	  bamWriteComplete(false),
	  TP(rTP)
	{
		for ( uint64_t i = 0; i < inflateBases.size(); ++i )
		{
			libmaus::lz::BgzfInflateBase::unique_ptr_type tptr(new libmaus::lz::BgzfInflateBase);
			inflateBases[i] = UNIQUE_PTR_MOVE(tptr);
			inflateBasesFreeList.enque(i);
		}
		
		for ( uint64_t i = 0; i < processBuffers.size(); ++i )
		{
			BamProcessBuffer::unique_ptr_type tptr(new BamProcessBuffer(processBufferSize));
			processBuffers[i] = UNIQUE_PTR_MOVE(tptr);
			processBuffersFreeList.push_back(i);
		}
		
		for ( uint64_t i = 0; i < numthreads; ++i )
		{
			std::ostringstream ostr;
			ostr << tmpfilenamebase << "_" << std::setw(6) << std::setfill('0') << i << std::setw(0) << ".sorttmp";
			std::string const fn = ostr.str();
			tmpfilenames[i] = fn;
			libmaus::util::TempFileRemovalContainer::addTempFile(fn);
			libmaus::aio::CheckedOutputStream::unique_ptr_type tptr(
				new libmaus::aio::CheckedOutputStream(fn)
			);
			tmpfiles[i] = UNIQUE_PTR_MOVE(tptr);
			
			libmaus::lz::SimpleCompressedOutputStream<std::ostream>::unique_ptr_type tcptr(
				new libmaus::lz::SimpleCompressedOutputStream<std::ostream>(*tmpfiles[i],compressorFactory)
			);
			
			compressedTmpFiles[i] = UNIQUE_PTR_MOVE(tcptr);
		}
	}
	
	char * getDecompressSpace(uint64_t const rbaseid)
	{
		return inflateDecompressSpace.begin() + rbaseid * libmaus::lz::BgzfConstants::getBgzfMaxBlockSize();
	}
};


struct BamThreadPoolDecodeReadPackageDispatcher : public libmaus::parallel::SimpleThreadWorkPackageDispatcher
{
	virtual ~BamThreadPoolDecodeReadPackageDispatcher() {}
	virtual void dispatch(
		libmaus::parallel::SimpleThreadWorkPackage * P, 
		libmaus::parallel::SimpleThreadPoolInterfaceEnqueTermInterface & tpi
	)
	{
		assert ( dynamic_cast<BamThreadPoolDecodeReadPackage *>(P) != 0 );
		
		BamThreadPoolDecodeReadPackage & RP = *dynamic_cast<BamThreadPoolDecodeReadPackage *>(P);
		BamThreadPoolDecodeContextBase & contextbase = *(RP.contextbase);
		
		// handle empty input file
		if ( contextbase.readCnt.get() == 0 )
		{
			libmaus::parallel::ScopePosixSpinLock slock(contextbase.inputLock);
			
			if ( contextbase.in.peek() == std::istream::traits_type::eof() )
			{
				contextbase.readComplete.set(true);
				contextbase.decompressComplete.set(true);
				contextbase.bamParseComplete.set(true);
				contextbase.bamProcessComplete.set(true);
				contextbase.bamSortComplete.set(true);
				tpi.terminate();
			}
		}

		if ( ! contextbase.readComplete.get() )
		{
			assert ( contextbase.in.peek() != std::istream::traits_type::eof() );
			
			uint64_t baseid;

			while (  contextbase.inflateBasesFreeList.trydeque(baseid) )
			{
				uint64_t const nextInputBlockId = (contextbase.nextInputBlockId++);

				uint64_t readCnt;
				uint64_t readCompCnt;
				std::pair<uint64_t,uint64_t> blockmeta;
				
				{			
					libmaus::parallel::ScopePosixSpinLock slock(contextbase.inputLock);
					blockmeta = contextbase.inflateBases[baseid]->readBlock(contextbase.in);

					readCnt = contextbase.readCnt++;
					contextbase.readCompCnt += blockmeta.first;
					readCompCnt = contextbase.readCompCnt.get();

					if ( contextbase.in.peek() == std::istream::traits_type::eof() )
					{
						contextbase.readComplete.set(true);
						contextbase.cerrlock.lock();
						std::cerr << "read complete." << std::endl;
						contextbase.cerrlock.unlock();
					}
				}
							
				if ( readCnt % 16384 == 0 )
				{
					contextbase.cerrlock.lock();
					std::cerr << "[D] input rate " << (readCompCnt / contextbase.inputRtc.getElapsedSeconds())/(1024.0*1024.0) << std::endl;
					contextbase.TP.printStateHistogram(std::cerr);
					contextbase.cerrlock.unlock();
				}
				
				BamThreadPoolDecodeDecompressPackage * pBTPDDP = RP.contextbase->decompressFreeList.getPackage();
				*pBTPDDP = BamThreadPoolDecodeDecompressPackage(0,RP.contextbase,blockmeta,baseid,nextInputBlockId);

				tpi.enque(pBTPDDP);
			}

			contextbase.readSem.post();
		}
		
		RP.contextbase->readFreeList.returnPackage(dynamic_cast<BamThreadPoolDecodeReadPackage *>(P));
	}
};

struct BamThreadPoolDecodeDecompressPackageDispatcher : public libmaus::parallel::SimpleThreadWorkPackageDispatcher
{
	virtual ~BamThreadPoolDecodeDecompressPackageDispatcher() {}
	virtual void dispatch(
		libmaus::parallel::SimpleThreadWorkPackage * P, 
		libmaus::parallel::SimpleThreadPoolInterfaceEnqueTermInterface & tpi
	)
	{
		assert ( dynamic_cast<BamThreadPoolDecodeDecompressPackage *>(P) != 0 );
		
		BamThreadPoolDecodeDecompressPackage & RP = *dynamic_cast<BamThreadPoolDecodeDecompressPackage *>(P);
		BamThreadPoolDecodeContextBase & contextbase = *(RP.contextbase);

		char * const decompressSpace = contextbase.getDecompressSpace(RP.baseid);
		contextbase.inflateBases[RP.baseid]->decompressBlock(decompressSpace,RP.blockmeta);

		#if 0
		contextbase.cerrlock.lock();
		std::cerr << "back block " << RP.blockid << std::endl;
		contextbase.cerrlock.unlock();
		#endif

		libmaus::parallel::ScopePosixSpinLock ldecompressLock(contextbase.decompressLock);
		contextbase.decompressCnt += 1;
		if ( contextbase.readComplete.get() && (contextbase.decompressCnt == contextbase.readCnt) )
		{
			contextbase.decompressComplete.set(true);

			#if 1
			contextbase.cerrlock.lock();
			std::cerr << "decompress complete." << std::endl;
			contextbase.cerrlock.unlock();
			#endif
		}

		{
			// generate parse info object
			BamThreadPoolDecodeBamParseQueueInfo qinfo(0,RP.blockmeta,RP.baseid,RP.blockid);

			// enque the parse info object
			libmaus::parallel::ScopePosixSpinLock lbamParseQueueLock(contextbase.bamParseQueueLock);
			contextbase.bamparseQueue.push(qinfo);
			
			// check whether this is the next block for parsing
			if ( contextbase.bamParseNextBlock == contextbase.bamparseQueue.top().blockid )
			{
				BamThreadPoolDecodeBamParseQueueInfo const bqinfo = contextbase.bamparseQueue.top();
				contextbase.bamparseQueue.pop();

				BamThreadPoolDecodeBamParsePackage BTPDBPP(
					bqinfo.packageid,
					RP.contextbase,
					bqinfo.blockmeta,
					bqinfo.baseid,
					bqinfo.blockid
				);
				
				BamThreadPoolDecodeBamParsePackage * pBTPDBPP = RP.contextbase->bamParseFreeList.getPackage();
				*pBTPDBPP = BamThreadPoolDecodeBamParsePackage(
					bqinfo.packageid,
					RP.contextbase,
					bqinfo.blockmeta,
					bqinfo.baseid,
					bqinfo.blockid
				);


				tpi.enque(pBTPDBPP);
			}
		}		

		RP.contextbase->decompressFreeList.returnPackage(dynamic_cast<BamThreadPoolDecodeDecompressPackage *>(P));
	}
};

struct BamThreadPoolDecodeBamParsePackageDispatcher : public libmaus::parallel::SimpleThreadWorkPackageDispatcher
{
	template<unsigned int len>
	static uint64_t decodeLittleEndianInteger(uint8_t const * pa)
	{
		#if defined(LIBMAUS_HAVE_i386)
		if ( len == 4 )
		{
			return *reinterpret_cast<uint32_t const *>(pa);
		}
		#else
		uint64_t v = 0;
		for ( unsigned int shift = 0; shift < 8*len; shift += 8 )
			v |= static_cast<uint64_t>(*(pa++)) << shift;
		return v;
		#endif
	}

	virtual ~BamThreadPoolDecodeBamParsePackageDispatcher() {}
	virtual void dispatch(
		libmaus::parallel::SimpleThreadWorkPackage * P, 
		libmaus::parallel::SimpleThreadPoolInterfaceEnqueTermInterface & tpi
	)
	{
		assert ( dynamic_cast<BamThreadPoolDecodeBamParsePackage *>(P) != 0 );
		
		BamThreadPoolDecodeBamParsePackage & RP = *dynamic_cast<BamThreadPoolDecodeBamParsePackage *>(P);
		BamThreadPoolDecodeContextBase & contextbase = *(RP.contextbase);
		
		#if 0
		contextbase.cerrlock.lock();
		std::cerr << "bamparse " << RP.blockid << std::endl;
		contextbase.cerrlock.unlock();
		#endif

		char * const decompressSpace = contextbase.getDecompressSpace(RP.baseid);

		uint8_t const * pa = reinterpret_cast<uint8_t const *>(decompressSpace);
		uint8_t const * pc = pa + RP.blockmeta.second;

		if ( (! contextbase.haveheader.get()) && (pa != pc) )
		{			
			::libmaus::util::GetObject<uint8_t const *> G(pa);
			std::pair<bool,uint64_t> const P = ::libmaus::bambam::BamHeader::parseHeader(G,contextbase.bamheaderparsestate,pc-pa);

			// header complete?
			if ( P.first )
			{
				contextbase.header.init(contextbase.bamheaderparsestate);
				contextbase.haveheader.set(true);
				pa += P.second;
				
				contextbase.cerrlock.lock();
				std::cerr << contextbase.header.text;
				contextbase.cerrlock.unlock();
			}
			else
			{
				pa += P.second;
			}
		}

		bool bufferAvailable = !(contextbase.processBuffersFreeList.empty()); 
		int64_t freeBufferId = bufferAvailable ? contextbase.processBuffersFreeList.dequeFront() : -1;
		std::vector<uint64_t> finishedBuffers;
		
		#if 0
		contextbase.cerrlock.lock();
		std::cerr << "bamparse using free buffer id " << freeBufferId << std::endl;
		contextbase.cerrlock.unlock();
		#endif

		bool stall = freeBufferId < 0;
		bool running = (pa != pc) && (!stall);
		
		while ( running )
		{
			assert ( contextbase.haveheader.get() );
			BamProcessBuffer * bamProcessBuffer = contextbase.processBuffers[freeBufferId].get();
			
			switch ( contextbase.bamParserState )
			{
				case contextbase.bam_parser_state_read_blocklength:
				{
					if ( contextbase.bamBlockSizeRead == 0 )
					{
						uint32_t lblocksize;
						while ( (pc-pa >= 4) && ((pc-pa) >= (4+(lblocksize=decodeLittleEndianInteger<4>(pa)) )) )
						{
							#if 0
							contextbase.cerrlock.lock();
							std::cerr << "lblocksize=" << lblocksize << std::endl;
							contextbase.cerrlock.unlock();
							#endif
							if ( bamProcessBuffer->put(pa+4,lblocksize) )
							{
								pa += 4 + lblocksize;
							}
							else
							{
								finishedBuffers.push_back(freeBufferId);
								
								bufferAvailable = !(contextbase.processBuffersFreeList.empty());
								freeBufferId = bufferAvailable ? contextbase.processBuffersFreeList.dequeFront() : -1;
								
								if ( freeBufferId < 0 )
								{
									running = false;
									stall = true;
									break;
								}
								else
								{
									bamProcessBuffer = contextbase.processBuffers[freeBufferId].get();
								}
							}
						}						
					}
					while ( running && (pa != pc) && (contextbase.bamBlockSizeRead < 4) )
					{
						contextbase.bamBlockSizeValue |= static_cast<uint32_t>(*(pa++)) << (8*((contextbase.bamBlockSizeRead++)));
					}
					if ( running && (contextbase.bamBlockSizeRead == 4) )
					{
						contextbase.bamParserState = contextbase.bam_parser_state_read_blockdata;
						contextbase.bamBlockDataRead = 0;

						#if 0
						contextbase.cerrlock.lock();
						std::cerr << "lblocksize2=" << contextbase.bamBlockSizeValue << std::endl;
						contextbase.cerrlock.unlock();
						#endif

						if ( contextbase.bamBlockSizeValue > contextbase.bamGatherBuffer.D.size() )
							contextbase.bamGatherBuffer.D = libmaus::bambam::BamAlignment::D_array_type(contextbase.bamBlockSizeValue);
					}
					break;
				}
				case contextbase.bam_parser_state_read_blockdata:
				{
					assert ( pa != pc );
				
					uint64_t const skip = 
						std::min(
							static_cast<uint64_t>(pc-pa),
							static_cast<uint64_t>(contextbase.bamBlockSizeValue - contextbase.bamBlockDataRead)
						);
							
					std::copy(pa,pa+skip,contextbase.bamGatherBuffer.D.begin()+contextbase.bamBlockDataRead);
						
					contextbase.bamBlockDataRead += skip;
					pa += skip;

					if ( contextbase.bamBlockDataRead == contextbase.bamBlockSizeValue )
					{
						contextbase.bamGatherBuffer.blocksize = contextbase.bamBlockSizeValue;

						if ( bamProcessBuffer->put(
							contextbase.bamGatherBuffer.D.begin(),
							contextbase.bamGatherBuffer.blocksize)
						)
						{
							contextbase.bamBlockSizeRead = 0;
							contextbase.bamBlockSizeValue = 0;
							contextbase.bamParserState = contextbase.bam_parser_state_read_blocklength;
						}
						else
						{
							// queue finished buffer
							finishedBuffers.push_back(freeBufferId);

							// rewind to process this data again
							pa -= skip;
							contextbase.bamBlockDataRead -= skip;

							// check for next free buffer								
							bufferAvailable = !(contextbase.processBuffersFreeList.empty());
							freeBufferId = bufferAvailable ? contextbase.processBuffersFreeList.dequeFront() : -1;

							// no more free buffers, stall
							if ( freeBufferId < 0 )
							{
								stall = true;
								running = false;
							}
							else
								bamProcessBuffer = contextbase.processBuffers[freeBufferId].get();							
						}
					}
					break;
				}
			}
			
			running = running && (pa != pc) && (!stall);
		}

		if ( ! stall )
		{
			// add inflate object to free list if it is no longer required
			contextbase.inflateBasesFreeList.enque(RP.baseid);

			// enque next read package when stream is ready for reading
			if ( contextbase.readSem.trywait() )
			{
				BamThreadPoolDecodeReadPackage * pBTPDRP = RP.contextbase->readFreeList.getPackage();
				*pBTPDRP = BamThreadPoolDecodeReadPackage(0,RP.contextbase);
				tpi.enque(pBTPDRP);
			}
		}

		if ( ! stall )
		{
			
			libmaus::parallel::ScopePosixSpinLock lbamParseLock(contextbase.bamParseLock);
			contextbase.bamParseCnt += 1;
			if ( contextbase.decompressComplete.get() && (contextbase.bamParseCnt == contextbase.decompressCnt) )
			{
				contextbase.cerrlock.lock();
				std::cerr << "bamParse complete." << std::endl;
				contextbase.cerrlock.unlock();
				contextbase.bamParseComplete.set(true);

				// queue final finished buffer
				finishedBuffers.push_back(freeBufferId);
			}
			else
			{
				// reinsert unfinished buffer
				contextbase.processBuffersFreeList.push_front(freeBufferId);	
			}
		}

		if ( stall )
		{
			// move remaining data to start of buffer
			memmove(reinterpret_cast<uint8_t *>(decompressSpace),pa,pc-pa);

			RP.blockmeta.second = pc-pa;

			#if 1
			contextbase.cerrlock.lock();
			std::cerr << "stalling, rest " << pc-pa << std::endl;
			contextbase.cerrlock.unlock();
			#endif
		
			// stall package
			BamThreadPoolDecodeBamParseQueueInfo qinfo(
				RP.packageid,
				RP.blockmeta,
				RP.baseid,
				RP.blockid
			);

			// enque the parse info object in the stall queue
			libmaus::parallel::ScopePosixSpinLock lbamParseQueueLock(contextbase.bamParseQueueLock);
			contextbase.bamparseStall.push(qinfo);
		}

		// queue finished buffers
		if ( finishedBuffers.size() )
		{
			for ( uint64_t i = 0; i < finishedBuffers.size(); ++i )
			{
				contextbase.cerrlock.lock();
				std::cerr << "queueing process block." << std::endl;
				contextbase.cerrlock.unlock();
			
				BamThreadPoolDecodeBamProcessQueueInfo qinfo(
					0,
					finishedBuffers[i],
					contextbase.nextProcessBufferIdIn++	
				);

				BamThreadPoolDecodeBamProcessPackage * pprocpack = RP.contextbase->bamProcessFreeList.getPackage();
				*pprocpack = BamThreadPoolDecodeBamProcessPackage(
					qinfo.packageid,
					RP.contextbase,
					qinfo.baseid,
					qinfo.blockid
				);
				
				tpi.enque(pprocpack);
			}			
		}
		
		#if 0
		contextbase.cerrlock.lock();
		std::cerr << "parse finished " << finishedBuffers.size() << " buffers" << std::endl;
		contextbase.cerrlock.unlock();
		#endif

		if ( ! stall )
		{
			// process next bam parse object if any is available
			{
				libmaus::parallel::ScopePosixSpinLock lbamParseQueueLock(contextbase.bamParseQueueLock);
				contextbase.bamParseNextBlock += 1;

				if ( contextbase.bamParseNextBlock == contextbase.bamparseQueue.top().blockid )
				{
					BamThreadPoolDecodeBamParseQueueInfo const bqinfo = contextbase.bamparseQueue.top();
					contextbase.bamparseQueue.pop();

					BamThreadPoolDecodeBamParsePackage * pBTPDBPP = RP.contextbase->bamParseFreeList.getPackage();
					*pBTPDBPP = BamThreadPoolDecodeBamParsePackage(
						bqinfo.packageid,
						RP.contextbase,
						bqinfo.blockmeta,
						bqinfo.baseid,
						bqinfo.blockid
					);
					
					tpi.enque(pBTPDBPP);
				}
			}
		}

		RP.contextbase->bamParseFreeList.returnPackage(dynamic_cast<BamThreadPoolDecodeBamParsePackage *>(P));
	}
};


struct BamThreadPoolDecodeBamProcessPackageDispatcher : public libmaus::parallel::SimpleThreadWorkPackageDispatcher
{
	virtual ~BamThreadPoolDecodeBamProcessPackageDispatcher() {}
	virtual void dispatch(
		libmaus::parallel::SimpleThreadWorkPackage * P, 
		libmaus::parallel::SimpleThreadPoolInterfaceEnqueTermInterface & tpi
	)
	{
		assert ( dynamic_cast<BamThreadPoolDecodeBamProcessPackage *>(P) != 0 );
		
		BamThreadPoolDecodeBamProcessPackage & RP = *dynamic_cast<BamThreadPoolDecodeBamProcessPackage *>(P);
		BamThreadPoolDecodeContextBase & contextbase = *(RP.contextbase);

		#if 0		
		contextbase.cerrlock.lock();
		std::cerr << "bamprocess processing " << RP.blockid << std::endl;
		contextbase.cerrlock.unlock();
		#endif

		BamProcessBuffer * processBuffer = contextbase.processBuffers[RP.baseid].get();
		std::reverse(processBuffer->pc,processBuffer->pa);
		
		#if 0
			libmaus::bambam::BamAlignmentPosComparator BAPC(processBuffer->ca);
			std::stable_sort(processBuffer->pc,processBuffer->pa,BAPC);

			#if 1
			contextbase.cerrlock.lock();
			std::cerr << "sorted " << (processBuffer->pa-processBuffer->pc) << std::endl;
			contextbase.cerrlock.unlock();
			#endif

			// return buffer to free list
			contextbase.processBuffers[RP.baseid]->reset();
			contextbase.processBuffersFreeList.push_back(RP.baseid);
			
			// increment number of processed buffers
			{
			libmaus::parallel::ScopePosixSpinLock lnextProcessBufferIdOutLock(contextbase.nextProcessBufferIdOutLock);
			contextbase.nextProcessBufferIdOut += 1;
			}

			// reinsert stalled parse package
			{
				libmaus::parallel::ScopePosixSpinLock lbamParseQueueLock(contextbase.bamParseQueueLock);
				
				if ( contextbase.bamparseStall.size() )
				{
					BamThreadPoolDecodeBamParseQueueInfo const bqinfo = contextbase.bamparseStall.top();
					contextbase.bamparseStall.pop();

					BamThreadPoolDecodeBamParsePackage * pBTPDBPP = RP.contextbase->bamParseFreeList.getPackage();
					*pBTPDBPP = BamThreadPoolDecodeBamParsePackage(
						bqinfo.packageid,
						RP.contextbase,
						bqinfo.blockmeta,
						bqinfo.baseid,
						bqinfo.blockid
					);

					contextbase.cerrlock.lock();
					std::cerr << "reinserting stalled block " << bqinfo.blockid << std::endl;
					contextbase.cerrlock.unlock();
		
					tpi.enque(pBTPDBPP);			
				}
			}

			// check whether processing is complete
			{
				libmaus::parallel::ScopePosixSpinLock lnextProcessBufferIdOutLock(contextbase.nextProcessBufferIdOutLock);
				if ( 
					contextbase.bamParseComplete.get() && (contextbase.nextProcessBufferIdOut == contextbase.nextProcessBufferIdIn) 
				)
				{
					contextbase.bamProcessComplete.set(true);
					std::cerr << "bamProcess complete" << std::endl;
					tpi.terminate();
				}
			}

			RP.contextbase->bamProcessFreeList.returnPackage(dynamic_cast<BamThreadPoolDecodeBamProcessPackage *>(P));
		#else
			BamSortInfo::shared_ptr_type sortinfo(new BamSortInfo(processBuffer,contextbase.TP.threads.size()));

			for ( uint64_t i = 0; i < contextbase.TP.threads.size(); ++i )
			{
				BamThreadPoolDecodeBamSortPackage * spack = RP.contextbase->bamSortFreeList.getPackage();
				*spack = BamThreadPoolDecodeBamSortPackage(
					0, RP.contextbase, RP.baseid, RP.blockid, sortinfo, BamThreadPoolDecodeBamSortPackage::sort_package_type_base,
					i,0,0
				);
				tpi.enque(spack);
			}

			// increment number of processed buffers
			{
			libmaus::parallel::ScopePosixSpinLock lnextProcessBufferIdOutLock(contextbase.nextProcessBufferIdOutLock);
			contextbase.nextProcessBufferIdOut += 1;
			}

			// check whether processing is complete
			{
				libmaus::parallel::ScopePosixSpinLock lnextProcessBufferIdOutLock(contextbase.nextProcessBufferIdOutLock);
				if ( 
					contextbase.bamParseComplete.get() && (contextbase.nextProcessBufferIdOut == contextbase.nextProcessBufferIdIn) 
				)
				{
					contextbase.bamProcessComplete.set(true);
					std::cerr << "bamProcess complete" << std::endl;
					// tpi.terminate();
				}
			}

			// return package
			RP.contextbase->bamProcessFreeList.returnPackage(dynamic_cast<BamThreadPoolDecodeBamProcessPackage *>(P));
		#endif
	}
};

struct BamThreadPoolDecodeBamSortPackageDispatcher : public libmaus::parallel::SimpleThreadWorkPackageDispatcher
{
	virtual ~BamThreadPoolDecodeBamSortPackageDispatcher() {}
	virtual void dispatch(
		libmaus::parallel::SimpleThreadWorkPackage * P, 
		libmaus::parallel::SimpleThreadPoolInterfaceEnqueTermInterface & tpi
	)
	{
		assert ( dynamic_cast<BamThreadPoolDecodeBamSortPackage *>(P) != 0 );
		
		BamThreadPoolDecodeBamSortPackage & RP = *dynamic_cast<BamThreadPoolDecodeBamSortPackage *>(P);
		BamThreadPoolDecodeContextBase & contextbase = *(RP.contextbase);
		BamSortInfo::sort_type & sortControl = *(RP.sortinfo->sortControl);
		
		#if 0
		contextbase.cerrlock.lock();
		std::cerr << "BamThreadPoolDecodeBamSortPackageDispatcher " 
			<< "packagetype=" << static_cast<int>(RP.packagetype) 
			<< ",sort_base_id=" << RP.sort_base_id
			<< ",sort_merge_id=" << RP.sort_merge_id
			<< ",sort_submerge_id=" << RP.sort_submerge_id
			<< std::endl;
		contextbase.cerrlock.unlock();
		#endif
		
		switch ( RP.packagetype )
		{
			case BamThreadPoolDecodeBamSortPackage::sort_package_type_base:
			{
				libmaus::sorting::ParallelStableSort::BaseSortRequestSet<uint64_t *,BamSortInfo::order_type> & baseSortRequests =
					sortControl.baseSortRequests;

				baseSortRequests.baseSortRequests[RP.sort_base_id].dispatch();

				uint64_t const finished = ++(baseSortRequests.requestsFinished);

				if ( finished == baseSortRequests.baseSortRequests.size() )
				{
					#if 0
					contextbase.cerrlock.lock();
					std::cerr << "BamThreadPoolDecodeBamSortPackageDispatcher done" << std::endl;
					contextbase.cerrlock.unlock();	
					#endif

					BamThreadPoolDecodeBamSortPackage * spack = RP.contextbase->bamSortFreeList.getPackage();
					*spack = BamThreadPoolDecodeBamSortPackage(0, RP.contextbase, RP.baseid, RP.blockid, 
						RP.sortinfo, BamThreadPoolDecodeBamSortPackage::sort_package_type_merge_level,0,0,0);
					tpi.enque(spack);
				}
				break;
			}
			case BamThreadPoolDecodeBamSortPackage::sort_package_type_merge_level:
			{
				if ( sortControl.mergeLevels.levelsFinished.get() == sortControl.mergeLevels.levels.size() )
				{
					contextbase.cerrlock.lock();
					std::cerr << "sorted buffer " << contextbase.processBuffers[RP.baseid]->pa-contextbase.processBuffers[RP.baseid]->pc << std::endl;
					contextbase.cerrlock.unlock();

					BamProcessBuffer * processBuffer = contextbase.processBuffers[RP.baseid].get();
					libmaus::bambam::BamAlignmentPosComparator BAPC(processBuffer->ca);
					
					BamProcessBuffer::pointer_type * pa =
						sortControl.needCopyBack ?
						( processBuffer->pc - (processBuffer->pa-processBuffer->pc) )
						:
						processBuffer->pc;
					BamProcessBuffer::pointer_type * pe =
						sortControl.needCopyBack ? processBuffer->pc : processBuffer->pa;

					uint64_t const numthreads = contextbase.TP.threads.size();
					std::vector < std::pair<BamProcessBuffer::pointer_type const *, BamProcessBuffer::pointer_type const *> > writepackets(numthreads);
					uint64_t const numalgn = pe-pa;
					uint64_t const prealperpack = (numalgn + numthreads - 1)/numthreads;
					uint64_t const alperpack =
						((prealperpack + contextbase.alperpackalign - 1)/contextbase.alperpackalign) * contextbase.alperpackalign;
					uint64_t allow = 0;

					for ( uint64_t i = 0; i < numthreads; ++i )
					{
						uint64_t const alhigh = std::min(allow+alperpack,numalgn);
						
						writepackets[i] = std::pair<BamProcessBuffer::pointer_type const *, BamProcessBuffer::pointer_type const *>(
							pa + allow, pa + alhigh
						);
						
						allow = alhigh;
					}

					BamBlockWriteInfo::shared_ptr_type blockWriteInfo(new BamBlockWriteInfo(processBuffer,writepackets));

					{
						libmaus::parallel::ScopePosixSpinLock ltmpfileblockslock(contextbase.tmpfileblockslock);

						while ( ! (RP.blockid < contextbase.tmpfileblocks.size() ) )
							contextbase.tmpfileblocks.push_back(
								std::vector< 
									std::pair<
										std::pair<uint64_t,uint64_t> ,
										std::pair<uint64_t,uint64_t>
									>
								>(
									numthreads
								)
							);
						
						while ( ! (RP.blockid < contextbase.tmpfileblockcnts.size() ) )
						{
							contextbase.tmpfileblockcnts.push_back(
								std::vector<uint64_t>(numthreads)
							);						
						}
						
						while ( ! (RP.blockid < contextbase.tmpfileblockcntsums.size() ) )
						{
							contextbase.tmpfileblockcntsums.push_back(0);
						}
					}

					libmaus::parallel::ScopePosixSpinLock lwritesPendingLock(contextbase.writesPendingLock);
					for ( uint64_t i = 0; i < numthreads; ++i )
					{
						BamThreadPoolDecodeBamWritePackage * pBTPDBPP = RP.contextbase->bamWriteFreeList.getPackage();
						*pBTPDBPP = BamThreadPoolDecodeBamWritePackage(
							0,RP.contextbase,RP.baseid,RP.blockid,
							blockWriteInfo,
							i
						);
						contextbase.writesPending[i].push(pBTPDBPP);
					}
					for ( uint64_t i = 0; i < numthreads; ++i )
					{
						assert ( ! contextbase.writesPending[i].empty() );
						
						if ( 
							contextbase.writesPending[i].top()->blockid ==
							contextbase.writesNext[i]
						)
						{
							BamThreadPoolDecodeBamWritePackage * pBTPDBPP = 
								contextbase.writesPending[i].top();
							contextbase.writesPending[i].pop();
							
							tpi.enque(pBTPDBPP);
						}
					}

					#if 0
					contextbase.cerrlock.lock();
					std::cerr << "needCopyBack " << sortControl.needCopyBack << std::endl;
					contextbase.cerrlock.unlock();
					
					contextbase.cerrlock.lock();	
					uint8_t const * prev = 0;
					for ( BamProcessBuffer::pointer_type * pc = pa ; pc != pe; ++pc )
					{
						uint8_t const * c = processBuffer->ca + (*pc)+4;
						#if 0
						std::cerr 
							<< libmaus::bambam::BamAlignmentDecoderBase::getReadName(c) 
							<< "\t"
							<< libmaus::bambam::BamAlignmentDecoderBase::getRefID(c)
							<< "\t"
							<< libmaus::bambam::BamAlignmentDecoderBase::getPos(c)
							<< std::endl;
						#endif
							
						if ( prev )
						{
							assert ( ! (BAPC( *pc , *(pc-1) )) );
							// std::cerr << BAPC( *pc , *(pc-1) ) << std::endl;
						}
							
						prev = c;
					}
					contextbase.cerrlock.unlock();
					#endif
					
					#if 0
					// return buffer to free list
					contextbase.processBuffers[RP.baseid]->reset();
					contextbase.processBuffersFreeList.push_back(RP.baseid);
					
					// reinsert stalled parse package
					{
						libmaus::parallel::ScopePosixSpinLock lbamParseQueueLock(contextbase.bamParseQueueLock);
						
						if ( contextbase.bamparseStall.size() )
						{
							BamThreadPoolDecodeBamParseQueueInfo const bqinfo = contextbase.bamparseStall.top();
							contextbase.bamparseStall.pop();

							BamThreadPoolDecodeBamParsePackage * pBTPDBPP = RP.contextbase->bamParseFreeList.getPackage();
							*pBTPDBPP = BamThreadPoolDecodeBamParsePackage(
								bqinfo.packageid,
								RP.contextbase,
								bqinfo.blockmeta,
								bqinfo.baseid,
								bqinfo.blockid
							);

							contextbase.cerrlock.lock();
							std::cerr << "reinserting stalled block " << bqinfo.blockid << std::endl;
							contextbase.cerrlock.unlock();
				
							tpi.enque(pBTPDBPP);			
						}
					}
					#endif

					libmaus::parallel::ScopePosixSpinLock lnextProcessBufferIdOutLock(contextbase.nextProcessBufferIdOutLock);
					libmaus::parallel::ScopePosixSpinLock lbuffersSortedLock(contextbase.buffersSortedLock);					
					contextbase.buffersSorted += 1;
					if ( 
						contextbase.bamProcessComplete.get()
						&&
						contextbase.nextProcessBufferIdOut == contextbase.buffersSorted
					)
					{
						contextbase.cerrlock.lock();
						std::cerr << "bamSort complete" << std::endl;
						contextbase.cerrlock.unlock();

						contextbase.bamSortComplete.set(true);
						// tpi.terminate();
					}
				}
				else
				{
					// search split points
					sortControl.mergeLevels.levels[RP.sort_merge_id].dispatch();
					
					// call merging
					for ( uint64_t i = 0; i < sortControl.mergeLevels.levels[RP.sort_merge_id].mergeRequests.size(); ++i )
					{
						BamThreadPoolDecodeBamSortPackage * spack = RP.contextbase->bamSortFreeList.getPackage();
						*spack = BamThreadPoolDecodeBamSortPackage(0, RP.contextbase, RP.baseid, RP.blockid, 
							RP.sortinfo, 
							BamThreadPoolDecodeBamSortPackage::sort_package_type_merge_sublevel,
							0,RP.sort_merge_id,i);
						tpi.enque(spack);	
					}
				}
				break;
			}
			case BamThreadPoolDecodeBamSortPackage::sort_package_type_merge_sublevel:
			{
				sortControl.mergeLevels.levels[RP.sort_merge_id].mergeRequests[RP.sort_submerge_id].dispatch();

				uint64_t const finished = ++(sortControl.mergeLevels.levels[RP.sort_merge_id].requestsFinished);
				
				if ( finished == sortControl.mergeLevels.levels[RP.sort_merge_id].mergeRequests.size() )
				{
					sortControl.mergeLevels.levelsFinished++;

					BamThreadPoolDecodeBamSortPackage * spack = RP.contextbase->bamSortFreeList.getPackage();
					*spack = BamThreadPoolDecodeBamSortPackage(0, RP.contextbase, RP.baseid, RP.blockid, 
						RP.sortinfo, 
						BamThreadPoolDecodeBamSortPackage::sort_package_type_merge_level,
						0,sortControl.mergeLevels.levelsFinished.get(),0
					);
					tpi.enque(spack);	
				}
				
				break;
			}
		}

		RP.sortinfo.reset();
		RP.contextbase->bamSortFreeList.returnPackage(dynamic_cast<BamThreadPoolDecodeBamSortPackage *>(P));		
	}
};

struct BamThreadPoolDecodeBamWritePackageDispatcher : public libmaus::parallel::SimpleThreadWorkPackageDispatcher
{
	virtual ~BamThreadPoolDecodeBamWritePackageDispatcher() {}
	virtual void dispatch(
		libmaus::parallel::SimpleThreadWorkPackage * P, 
		libmaus::parallel::SimpleThreadPoolInterfaceEnqueTermInterface & tpi
	)
	{
		assert ( dynamic_cast<BamThreadPoolDecodeBamWritePackage *>(P) != 0 );
		
		BamThreadPoolDecodeBamWritePackage & RP = *dynamic_cast<BamThreadPoolDecodeBamWritePackage *>(P);
		BamThreadPoolDecodeContextBase & contextbase = *(RP.contextbase);
		BamBlockWriteInfo * writeinfo = RP.writeinfo.get();		
		BamProcessBuffer * buffer = writeinfo->processBuffer;
		
		uint8_t const * ca = buffer->ca;
		BamProcessBuffer::pointer_type const * pa = writeinfo->packets[RP.write_block_id].first;
		BamProcessBuffer::pointer_type const * pe = writeinfo->packets[RP.write_block_id].second;
		libmaus::lz::SimpleCompressedOutputStream<std::ostream> * compout =
			contextbase.compressedTmpFiles[RP.write_block_id].get();
	
		std::pair<uint64_t,uint64_t> const preoff = compout->getOffset();
		assert ( preoff.second == 0 );
		for ( BamProcessBuffer::pointer_type const * pc = pa; pc != pe; ++pc )
		{
			uint64_t const off = *pc;
			uint8_t const * c = ca + off;
			
			uint32_t len = 0;
			for ( unsigned int i = 0; i < 4; ++i )
				len |= (static_cast<uint32_t>(c[i]) << (i*8));
			
			if ( (pc-pa) % contextbase.alperpackalign == 0 )
			{
				compout->flush();
				assert ( compout->getOffset().second == 0 );
			}
			compout->write(reinterpret_cast<char const *>(c),len+4);
		}
		std::pair<uint64_t,uint64_t> const postoff = compout->getOffset();
		compout->flush();

		contextbase.cerrlock.lock();
		std::cerr << "BamWrite " 
			<< RP.blockid << "," << RP.write_block_id 
			<< "," << (writeinfo->packets[RP.write_block_id].second-writeinfo->packets[RP.write_block_id].first)
			<< std::endl;
		contextbase.cerrlock.unlock();

		{
			libmaus::parallel::ScopePosixSpinLock ltmpfileblockslock(contextbase.tmpfileblockslock);
			contextbase.tmpfileblocks[RP.blockid][RP.write_block_id] =
				std::pair<
					std::pair<uint64_t,uint64_t>,
					std::pair<uint64_t,uint64_t>
				>(preoff,postoff);
			contextbase.tmpfileblockcnts[RP.blockid][RP.write_block_id] = (pe-pa);
			contextbase.tmpfileblockcntsums[RP.blockid] += (pe-pa);
		}

		// enque next block for stream writing (if any is present)
		{
			libmaus::parallel::ScopePosixSpinLock lwritesPendingLock(contextbase.writesPendingLock);
			contextbase.writesNext[RP.write_block_id]++;

			if ( 
				contextbase.writesPending.size() 
				&&
				contextbase.writesPending[RP.write_block_id].top()->blockid ==
				contextbase.writesNext[RP.write_block_id]
			)
			{
				BamThreadPoolDecodeBamWritePackage * pBTPDBPP = 
					contextbase.writesPending[RP.write_block_id].top();
				contextbase.writesPending[RP.write_block_id].pop();
				
				tpi.enque(pBTPDBPP);
			}
		}

		uint64_t const packetsWritten = ++(writeinfo->packetsWritten);
		
		if ( packetsWritten == writeinfo->packets.size() )
		{
			// return buffer to free list
			contextbase.processBuffers[RP.baseid]->reset();
			contextbase.processBuffersFreeList.push_back(RP.baseid);
					
			// reinsert stalled parse package
			{
				libmaus::parallel::ScopePosixSpinLock lbamParseQueueLock(contextbase.bamParseQueueLock);
				
				if ( contextbase.bamparseStall.size() )
				{
					BamThreadPoolDecodeBamParseQueueInfo const bqinfo = contextbase.bamparseStall.top();
					contextbase.bamparseStall.pop();

					BamThreadPoolDecodeBamParsePackage * pBTPDBPP = RP.contextbase->bamParseFreeList.getPackage();
					*pBTPDBPP = BamThreadPoolDecodeBamParsePackage(
						bqinfo.packageid,
						RP.contextbase,
						bqinfo.blockmeta,
						bqinfo.baseid,
						bqinfo.blockid
					);

					contextbase.cerrlock.lock();
					std::cerr << "reinserting stalled block " << bqinfo.blockid << std::endl;
					contextbase.cerrlock.unlock();
		
					tpi.enque(pBTPDBPP);			
				}
			}			

			libmaus::parallel::ScopePosixSpinLock lbuffersWrittenLock(contextbase.buffersWrittenLock);
			libmaus::parallel::ScopePosixSpinLock lbuffersSortedLock(contextbase.buffersSortedLock);
			contextbase.buffersWritten += 1;
			
			if ( 
				contextbase.bamSortComplete.get()
				&&
				contextbase.buffersWritten == contextbase.buffersSorted 
			)
			{
				for ( uint64_t i = 0; i < contextbase.compressedTmpFiles.size(); ++i )
				{
					contextbase.compressedTmpFiles[i]->flush();
					contextbase.compressedTmpFiles[i].reset();
					contextbase.tmpfiles[i]->flush();
					contextbase.tmpfiles[i].reset();
				}

				contextbase.cerrlock.lock();
				std::cerr << "bamWrite done." << std::endl;
				contextbase.cerrlock.unlock();
				
				contextbase.bamWriteComplete.set(true);
				tpi.terminate();
			}
		}

		// return package to free list
		RP.contextbase->bamWriteFreeList.returnPackage(dynamic_cast<BamThreadPoolDecodeBamWritePackage *>(P));
	}
};

struct BamThreadPoolDecodeContext : public BamThreadPoolDecodeContextBase
{
	libmaus::parallel::SimpleThreadPool & TP;

	BamThreadPoolDecodeContext(
		std::istream & rin, 
		uint64_t const numInflateBases,
		uint64_t const numProcessBuffers,
		uint64_t const processBufferSize,
		std::string const & tmpfilenamebase,
		uint64_t const numthreads,
		libmaus::parallel::SimpleThreadPool & rTP
	)
	: BamThreadPoolDecodeContextBase(rin,numInflateBases,numProcessBuffers,processBufferSize,tmpfilenamebase,numthreads,rTP), TP(rTP) 
	{
	
	}
	
	void startup()
	{
		inputRtc.start();
	
		assert ( BamThreadPoolDecodeContextBase::inflateBases.size() );

		BamThreadPoolDecodeReadPackage * pBTPDRP = readFreeList.getPackage();
		*pBTPDRP = BamThreadPoolDecodeReadPackage(0,this);
		TP.enque(pBTPDRP);
	}
};

#include <libmaus/aio/PosixFdInputStream.hpp>

int main(int argc, char * argv[])
{
	try
	{
		libmaus::util::ArgInfo const arginfo(argc,argv);
		std::string const tmpfilenamebase = arginfo.getDefaultTmpFileName();
	
		#if defined(_OPENMP)
		uint64_t const numthreads = omp_get_max_threads();
		#else
		uint64_t const numthreads = 1;
		#endif
		
		std::cerr << "numthreads=" << numthreads << std::endl;

		libmaus::parallel::SimpleThreadPool TP(numthreads);

		BamThreadPoolDecodeReadPackageDispatcher readdispatcher;
		TP.registerDispatcher(BamThreadPoolDecodeContext::bamthreadpooldecodecontextbase_dispatcher_id_read,&readdispatcher);
		BamThreadPoolDecodeDecompressPackageDispatcher decompressdispatcher;
		TP.registerDispatcher(BamThreadPoolDecodeContext::bamthreadpooldecodecontextbase_dispatcher_id_decompress,&decompressdispatcher);
		BamThreadPoolDecodeBamParsePackageDispatcher bamparsedispatcher;
		TP.registerDispatcher(BamThreadPoolDecodeContext::bamthreadpooldecodecontextbase_dispatcher_id_bamparse,&bamparsedispatcher);
		BamThreadPoolDecodeBamProcessPackageDispatcher bamprocessdispatcher;
		TP.registerDispatcher(BamThreadPoolDecodeContext::bamthreadpooldecodecontextbase_dispatcher_id_bamprocess,&bamprocessdispatcher);
		BamThreadPoolDecodeBamSortPackageDispatcher bamsortdispatcher;
		TP.registerDispatcher(BamThreadPoolDecodeContext::bamthreadpooldecodecontextbase_dispatcher_id_bamsort,&bamsortdispatcher);
		BamThreadPoolDecodeBamWritePackageDispatcher bamwritedispatcher;
		TP.registerDispatcher(BamThreadPoolDecodeContext::bamthreadpooldecodecontextbase_dispatcher_id_bamwrite,&bamwritedispatcher);

		uint64_t numProcessBuffers = 4; // 2*numthreads;
		uint64_t processBufferMemory = 16*1024ull*1024ull*1024ull;
		// uint64_t processBufferMemory = 512ull*1024ull;
		uint64_t processBufferSize = (processBufferMemory + numProcessBuffers-1)/numProcessBuffers;

		libmaus::aio::PosixFdInputStream PFIS(STDIN_FILENO,64*1024);
		BamThreadPoolDecodeContext context(PFIS,16*numthreads /* inflate bases */,numProcessBuffers,processBufferSize,tmpfilenamebase,numthreads,TP);
		context.startup();
		
		TP.join();

		std::cerr << "produced " << context.tmpfileblockcntsums.size() << " blocks: " << std::endl;
		for ( uint64_t i = 0; i < context.tmpfileblockcntsums.size(); ++i )
			std::cerr << "block[" << i << "]=" << context.tmpfileblockcntsums[i] << std::endl;
	}
	catch(std::exception const & ex)
	{
		std::cerr << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
