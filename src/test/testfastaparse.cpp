/*
    libmaus2
    Copyright (C) 2009-2015 German Tischler
    Copyright (C) 2011-2015 Genome Research Limited

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
#include <libmaus2/fastx/FastAStreamSet.hpp>
#include <libmaus2/fastx/FastALineParser.hpp>
#include <libmaus2/fastx/FastAMapTable.hpp>
#include <libmaus2/fastx/FastALineParserLineInfo.hpp>
#include <libmaus2/fastx/FastAMatchTable.hpp>
#include <libmaus2/timing/RealTimeClock.hpp>
#include <libmaus2/aio/PosixFdInputStream.hpp>

#include <libmaus2/wavelet/DynamicWaveletTree.hpp>
#include <libmaus2/random/Random.hpp>

#include <iostream>

int main()
{
	#if 0
	{
	::libmaus2::fastx::FastAStreamSet F(std::cin);
	std::map<std::string,std::string> const M = F.computeMD5();
	}
	#endif

	::libmaus2::fastx::FastAMatchTable FAMT;
	std::cerr << FAMT;

	libmaus2::timing::RealTimeClock rtc;
	rtc.start();

	::libmaus2::aio::PosixFdInputStream posin(STDIN_FILENO,64*1024);
	::libmaus2::fastx::FastAStreamSet FASS(posin);
	libmaus2::autoarray::AutoArray<char> B(64*1024,false);
	std::pair<std::string,::libmaus2::fastx::FastAStream::shared_ptr_type> P;
	uint64_t bases = 0;

	double const erate = 0.03;
	libmaus2::random::Random::setup();

	while ( FASS.getNextStream(P) )
	{
		libmaus2::wavelet::DynamicWaveletTree<8,8> D(2);

		std::string const & id = P.first;
		::libmaus2::fastx::FastAStream & basestream = *(P.second);

		uint64_t cnt = 0;
		while ( true )
		{
			basestream.read(B.begin(),B.size());
			uint64_t gcnt = basestream.gcount();

			for ( size_t i = 0; i < gcnt; ++i )
			{
				char const c = B[i];

				if ( c == 'A' )
					D.insert(0,cnt+i);
				else if ( c == 'C' )
					D.insert(1,cnt+i);
				else if ( c == 'G' )
					D.insert(2,cnt+i);
				else if ( c == 'T' )
					D.insert(3,cnt+i);
				else
					D.insert(libmaus2::random::Random::rand8()%4,cnt+i);
			}

			cnt += gcnt;

			std::cerr << "\r" << std::string(80,' ') << "\r" << cnt;

			if ( !gcnt )
				break;
		}

		std::cerr << "\r" << std::string(80,' ') << "\r" << cnt << "\n";

		uint64_t const errors = (D.size() * erate + 0.5);

		// std::cerr << "[V] Injecting " << errors << " errors." << std::endl;

		for ( uint64_t i = 0; i < errors; ++i )
		{
			unsigned int const errtype = libmaus2::random::Random::rand8() % 3;
			switch ( errtype )
			{
				case 0:
					{
						uint64_t const inpos = libmaus2::random::Random::rand64() % (D.size()+1);
						unsigned int const inbase = libmaus2::random::Random::rand8() % 4;
						D.insert(inbase,inpos);
						break;
					}
				case 1:
					if ( D.size() )
					{
						uint64_t const delpos = libmaus2::random::Random::rand64() % (D.size());
						D.remove(delpos);
					}
					break;
				case 2:
				{
					if ( D.size() )
					{
						uint64_t const subspos = libmaus2::random::Random::rand64() % (D.size());
						unsigned int const inbase = libmaus2::random::Random::rand8() % 4;
						D.remove(subspos);
						D.insert(inbase,subspos);
					}
					break;
				}
			}
		}

		std::cerr << id << "\t" << cnt << "\n";

		std::cout << ">" << P.first << "\n";
		for ( uint64_t i = 0; i < D.size(); ++i )
		{
			unsigned int const c = D[i];
			switch ( c )
			{
				case 0: std::cout.put('A'); break;
				case 1: std::cout.put('C'); break;
				case 2: std::cout.put('G'); break;
				case 3: std::cout.put('T'); break;
			}
		}
		std::cout.put('\n');

		bases += cnt;
	}

	std::cerr << (bases / rtc.getElapsedSeconds()) << " bases/s " << "(" << bases << "/" << rtc.getElapsedSeconds() << ")" << std::endl;
}
