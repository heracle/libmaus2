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
#if ! defined(LIBMAUS2_BAMBAM_BAMBLOCKWRITERBASEFACTORY_HPP)
#define LIBMAUS2_BAMBAM_BAMBLOCKWRITERBASEFACTORY_HPP

#include <libmaus2/bambam/BamWriter.hpp>
#if defined(LIBMAUS2_HAVE_IO_LIB)
#include <libmaus2/bambam/ScramEncoder.hpp>
#endif
#include <libmaus2/bambam/SamEncoder.hpp>
#include <libmaus2/util/OutputFileNameTools.hpp>

namespace libmaus2
{
	namespace bambam
	{
		struct BamBlockWriterBaseFactory
		{
			typedef BamBlockWriterBaseFactory this_type;
			typedef libmaus2::util::unique_ptr<this_type>::type unique_ptr_type;
			typedef libmaus2::util::shared_ptr<this_type>::type shared_ptr_type;

			BamBlockWriterBaseFactory() {}
			virtual ~BamBlockWriterBaseFactory() {}

			static std::string levelToString(int const level)
			{
				switch ( level )
				{
					case Z_DEFAULT_COMPRESSION:
						return "zlib default";
					case Z_BEST_SPEED:
						return "fast";
					case Z_BEST_COMPRESSION:
						return "best";
					case Z_NO_COMPRESSION:
						return "uncompressed";
					#if defined(LIBMAUS2_HAVE_IGZIP)
					case libmaus2::lz::IGzipDeflate::COMPRESSION_LEVEL:
						return "igzip";
					#endif
					default:
					{
						::libmaus2::exception::LibMausException se;
						se.getStream() << "BamBlockWriterBaseFactory::levelToString(): Unknown compression level " << level << std::endl;
						se.finish();
						throw se;
					}
				}
			}

			static std::string getIndexFileName(libmaus2::util::ArgInfo const & arginfo)
			{
				std::string const key = "indexfilename";

				if ( arginfo.hasArg(key) )
					return arginfo.getUnparsedValue(key,std::string());
				else if ( arginfo.hasArg("O") && arginfo.getUnparsedValue("O",std::string()) != std::string() )
				{
					std::string const O = arginfo.getUnparsedValue("O",std::string());

					char const * const suffixes[] = { ".sam", ".bam", ".cram", 0 };

					std::string const fn = libmaus2::util::OutputFileNameTools::endClip(O,&suffixes[0]) + ".bai";

					return fn;
				}
				else
					return std::string();
			}

			static std::string getMD5FileName(libmaus2::util::ArgInfo const & arginfo)
			{
				std::string const key = "md5filename";

				if ( arginfo.hasArg(key) )
					return arginfo.getUnparsedValue(key,std::string());
				else if ( arginfo.hasArg("O") && arginfo.getUnparsedValue("O",std::string()) != std::string() )
				{
					std::string const O = arginfo.getUnparsedValue("O",std::string());
					std::string const fn = O + ".md5";
					return fn;
				}
				else
					return std::string();
			}

			static std::string getLevelHelpText()
			{
				std::set<int> S = getValidCompressionLevels();
				std::vector<int> V(S.begin(),S.end());

				std::ostringstream ostr;
				for ( std::vector<int>::size_type i = 0; i < V.size(); ++i )
					ostr << V[i] << "=" << levelToString(V[i]) << ((i+1<V.size())?",":"");

				return ostr.str();
			}

			static std::string getBamOutputLevelHelpText()
			{
				return std::string("compression settings for output bam file (") + getLevelHelpText() + std::string(")");
			}

			static std::set<int> getValidCompressionLevels()
			{
				std::set<int> S;
				S.insert(Z_DEFAULT_COMPRESSION);
				S.insert(Z_BEST_SPEED);
				S.insert(Z_BEST_COMPRESSION);
				S.insert(Z_NO_COMPRESSION);
				#if defined(LIBMAUS2_HAVE_IGZIP)
				S.insert(libmaus2::lz::IGzipDeflate::getCompressionLevel());
				#endif
				return S;
			}

			static int checkCompressionLevel(int const level)
			{
				switch ( level )
				{
					case Z_NO_COMPRESSION:
					case Z_BEST_SPEED:
					case Z_BEST_COMPRESSION:
					case Z_DEFAULT_COMPRESSION:
					#if defined(LIBMAUS2_HAVE_IGZIP)
					case libmaus2::lz::IGzipDeflate::COMPRESSION_LEVEL:
					#endif
						break;
					default:
					{
						::libmaus2::exception::LibMausException se;
						se.getStream()
							<< "Unknown compression level, please use"
							<< " level=" << Z_DEFAULT_COMPRESSION << " (default) or"
							<< " level=" << Z_BEST_SPEED << " (fast) or"
							<< " level=" << Z_BEST_COMPRESSION << " (best) or"
							<< " level=" << Z_NO_COMPRESSION << " (no compression)"
							#if defined(LIBMAUS2_HAVE_IGZIP)
							<< " or level=" << libmaus2::lz::IGzipDeflate::COMPRESSION_LEVEL << " (igzip)"
							#endif
							<< std::endl;
						se.finish();
						throw se;
					}
						break;
				}

				return level;
			}

			static std::set<std::string> getValidOutputFormatsSet()
			{
				std::set<std::string> S;
				S.insert("bam");

				#if defined(LIBMAUS2_HAVE_IO_LIB)
				S.insert("sam");
				S.insert("cram");
				#endif

				return S;
			}

			static std::string getValidOutputFormats()
			{
				std::set<std::string> const S = getValidOutputFormatsSet();

				std::ostringstream ostr;
				for ( std::set<std::string>::const_iterator ita = S.begin();
					ita != S.end(); ++ita )
					ostr << ((ita!=S.begin())?",":"") << (*ita);

				return ostr.str();
			}

			static std::string getDefaultOutputFormat()
			{
				return "bam";
			}

			static libmaus2::bambam::BamBlockWriterBase::unique_ptr_type construct(
				libmaus2::bambam::BamHeader const & bamheader,
				libmaus2::util::ArgInfo const & arginfo,
				std::vector< ::libmaus2::lz::BgzfDeflateOutputCallback *> const * rblockoutputcallbacks = 0
			)
			{
				std::string const outputformat = arginfo.getValue<std::string>("outputformat",getDefaultOutputFormat());
				uint64_t const outputthreads = std::max(static_cast<uint64_t>(1),arginfo.getValue<uint64_t>("outputthreads",1));
				bool const outputisstdout = (!arginfo.hasArg("O")) || ( arginfo.getUnparsedValue("O","-") == std::string("-") );
				std::string const outputfilename = arginfo.getUnparsedValue("O","-");

				if ( (outputformat != "bam") && rblockoutputcallbacks && rblockoutputcallbacks->size() )
				{
					libmaus2::exception::LibMausException ex;
					ex.getStream() << "libmaus2::bambam::BamBlockWriterBaseFactory: output callbacks are not supported for output formats other than bam" << std::endl;
					ex.finish();
					throw ex;
				}

				if ( outputformat == "bam" )
				{
					int const level = checkCompressionLevel(arginfo.getValue("level",Z_DEFAULT_COMPRESSION));

					if ( outputthreads == 1 )
					{
						if ( outputisstdout )
						{
							libmaus2::bambam::BamBlockWriterBase::unique_ptr_type tptr(new libmaus2::bambam::BamWriter(std::cout,bamheader,level,rblockoutputcallbacks));
							return tptr;
						}
						else
						{
							libmaus2::bambam::BamBlockWriterBase::unique_ptr_type tptr(new libmaus2::bambam::BamWriter(outputfilename,bamheader,level,rblockoutputcallbacks));
							return tptr;
						}
					}
					else
					{
						if ( outputisstdout )
						{
							libmaus2::bambam::BamBlockWriterBase::unique_ptr_type tptr(new libmaus2::bambam::BamParallelWriter(std::cout,outputthreads,bamheader,level,rblockoutputcallbacks));
							return tptr;
						}
						else
						{
							libmaus2::bambam::BamBlockWriterBase::unique_ptr_type tptr(new libmaus2::bambam::BamParallelWriter(outputfilename,outputthreads,bamheader,level,rblockoutputcallbacks));
							return tptr;
						}
					}
				}
				else if (
					outputformat == "maussam"
					#if !defined(LIBMAUS2_HAVE_IO_LIB)
					||
					outputformat == "sam"
					#endif
				)
				{
					if ( outputisstdout )
					{
						libmaus2::bambam::BamBlockWriterBase::unique_ptr_type tptr(
							new libmaus2::bambam::SamEncoder(std::cout,bamheader)
						);
						return tptr;
					}
					else
					{
						libmaus2::bambam::BamBlockWriterBase::unique_ptr_type tptr(
							new libmaus2::bambam::SamEncoder(outputfilename,bamheader)
						);
						return tptr;
					}

				}
				#if defined(LIBMAUS2_HAVE_IO_LIB)
				else if ( outputformat == "sam" )
				{

					if ( outputisstdout )
					{
						libmaus2::bambam::BamBlockWriterBase::unique_ptr_type tptr(new libmaus2::bambam::ScramEncoder(bamheader,"-","ws","",true /* verbose */));
						return tptr;
					}
					else
					{
						libmaus2::bambam::BamBlockWriterBase::unique_ptr_type tptr(new libmaus2::bambam::ScramEncoder(bamheader,outputfilename,"ws","",true /* verbose */));
						return tptr;
					}
				}
				else if ( outputformat == "cram" )
				{
					std::string const reference = arginfo.getUnparsedValue("reference","");
					bool const scramverbose = arginfo.getValue<unsigned int>("scramverbose",false);

					if ( outputisstdout )
					{
						libmaus2::bambam::BamBlockWriterBase::unique_ptr_type tptr(new libmaus2::bambam::ScramEncoder(bamheader,"-","wc",reference,scramverbose /* verbose */));
						return tptr;
					}
					else
					{
						libmaus2::bambam::BamBlockWriterBase::unique_ptr_type tptr(new libmaus2::bambam::ScramEncoder(bamheader,outputfilename,"wc",reference,scramverbose /* verbose */));
						return tptr;
					}
				}
				#endif
				else
				{
					libmaus2::exception::LibMausException se;
					se.getStream() << "BamBlockWriterBaseFactory::construct(): unknown/unsupported output format " << outputformat << std::endl;
					se.finish();
					throw se;
				}
			}
		};
	}
}
#endif
