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

#if ! defined(LIBMAUS2_LCS_LCSREF_HPP)
#define LIBMAUS2_LCS_LCSREF_HPP

#include <string>
#include <libmaus2/types/types.hpp>

namespace libmaus2
{
	namespace lcs
	{
		struct LCSRef
		{
			static uint64_t lcs(std::string const & a, std::string const & b)
			{
				uint64_t maxlcp = 0;

				for ( uint64_t i = 0; i < a.size(); ++i )
					for ( uint64_t j = i+1; j <= a.size(); ++j )
					{
						std::string const q = a.substr(i,j-i);
						// std::cerr << "q=" << q << std::endl;
						if ( b.find(q) != std::string::npos )
							maxlcp = std::max(maxlcp,static_cast<uint64_t>(q.size()));
					}

				return maxlcp;
			}
		};
	}
}
#endif
