/*
    libmaus2
    Copyright (C) 2009-2013 German Tischler
    Copyright (C) 2011-2013 Genome Research Limited

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

#include <libmaus2/util/TempFileNameGeneratorState.hpp>

#include <cassert>
#include <sstream>
#include <algorithm>

/* for mkdir */
#include <sys/stat.h>
#include <sys/types.h>

#include <libmaus2/LibMausConfig.hpp>
#if defined(LIBMAUS2_HAVE_UNISTD_H)
#include <unistd.h>
#endif

#include <libmaus2/aio/OutputStreamFactoryContainer.hpp>

bool libmaus2::util::TempFileNameGeneratorState::operator==(libmaus2::util::TempFileNameGeneratorState const & o) const
{
	return
		depth == o.depth
		&&
		nextdir == o.nextdir
		&&
		nextfile == o.nextfile;
}

bool libmaus2::util::TempFileNameGeneratorState::operator!=(libmaus2::util::TempFileNameGeneratorState const & o) const
{
	return !((*this) == o);
}

static unsigned int computeDigits(unsigned int maxmod)
{
	if ( maxmod == 0 )
		return 1;

	unsigned int mod = 0;

	while ( maxmod )
	{
		mod += 1;
		maxmod /= 10;
	}

	return mod;
}

libmaus2::util::TempFileNameGeneratorState::TempFileNameGeneratorState(unsigned int const rdepth, std::string const & rprefix, unsigned int const rdirmod, unsigned int const rfilemod)
: dirmod(rdirmod), filemod(rfilemod), maxmod(std::max(dirmod,filemod)), digits(computeDigits(maxmod)), depth(rdepth), nextfile(-1), prefix(rprefix)
{
	assert ( depth );
	setup();
}

void libmaus2::util::TempFileNameGeneratorState::setup()
{
	nextdir = std::vector<int>(depth);
	std::fill ( nextdir.begin(), nextdir.end(), 0 );
	nextdir.back() = -1;
}


void libmaus2::util::TempFileNameGeneratorState::next()
{
	if ( (++nextfile) % filemod == 0 )
	{
		unsigned int idx = nextdir.size()-1;

		while ( (++ nextdir[idx]) == dirmod )
		{
			nextdir[idx] = 0;

			if ( ! idx )
			{
				--nextfile;
				depth += 1;
				setup();
				next();
			}

			--idx;
		}
	}
}

std::string libmaus2::util::TempFileNameGeneratorState::numToString(uint64_t const num, unsigned int dig)
{
	std::ostringstream ostr;
	ostr << std::setw(dig) << std::setfill('0') << num;
	return ostr.str();
}

std::string libmaus2::util::TempFileNameGeneratorState::getFileName()
{
	next();

	std::vector < std::string > particles;

	for ( uint64_t i = 0; i < nextdir.size(); ++i )
		particles.push_back( numToString(nextdir[i],digits) );

	std::string dirname = prefix;

	libmaus2::aio::OutputStreamFactoryContainer::mkdir(dirname,0700);

	for ( uint64_t i = 0; i < particles.size(); ++i )
	{
		dirname += "/";
		dirname += particles[i];
		libmaus2::aio::OutputStreamFactoryContainer::mkdir(dirname,0700);
	}

	std::ostringstream fnostr;
	fnostr << dirname << "/" << "file" << numToString(nextfile,digits);
	std::string const fn = fnostr.str();

	return fn;
}

std::vector < std::string > libmaus2::util::TempFileNameGeneratorState::removeDirs(std::vector < std::string > const & prevdirs)
{
	next();

	std::vector < std::string > particles;

	for ( uint64_t i = 0; i < nextdir.size(); ++i )
		particles.push_back(numToString(nextdir[i],digits));

	std::vector < std::string > dirs;

	std::string dirname = prefix;
	dirs.push_back(dirname);

	for ( uint64_t i = 0; i < particles.size(); ++i )
	{
		dirname += "/";
		dirname += particles[i];
		dirs.push_back(dirname);
	}

	if ( dirs != prevdirs )
	{
		std::reverse(dirs.begin(),dirs.end());
		for (
			::std::vector<std::string>::const_iterator ita = dirs.begin();
			ita != dirs.end();
			++ita )
		{
			// std::cerr << "Removing dir " << *ita << std::endl;
			rmdir ( ita->c_str() );
		}
		std::reverse(dirs.begin(),dirs.end());
	}

	return dirs;

}
