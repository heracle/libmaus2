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
#if ! defined(LIBMAUS2_DIGEST_CRC32_HPP)
#define LIBMAUS2_DIGEST_CRC32_HPP

#include <libmaus2/digest/DigestBase.hpp>
#include <libmaus2/lz/ZlibInterface.hpp>

namespace libmaus2
{
	namespace digest
	{
		struct CRC32 : public DigestBase<4 /* digest length */, 0 /* block size shift */, 0 /* need padding */, 0 /* number length */, false>
		{
			libmaus2::lz::ZlibInterface::unique_ptr_type zintf;
			uint32_t const initial;
			uint32_t ctx;

			CRC32();
			~CRC32();

			void init();
			void update(uint8_t const * t, size_t l);
			void digest(uint8_t * digest);
			void copyFrom(CRC32 const & O);
			static size_t getDigestLength() { return digestlength; }

			void vinit();
			void vupdate(uint8_t const * u, size_t l);
		};
	}
}
#endif
