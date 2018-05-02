/*
 * Copyright (C) 2011-2016 Daniel Scharrer
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the author(s) be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 */

#include "cli/extract.hpp"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <vector>
#include <cmath>

#include <boost/foreach.hpp>
#include <boost/scoped_ptr.hpp>
#include <boost/unordered_map.hpp>
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/ptr_container/ptr_vector.hpp>
#include <boost/range/size.hpp>

#include <boost/version.hpp>
#if BOOST_VERSION >= 104800
#include <boost/container/flat_map.hpp>
#endif

#include "cli/debug.hpp"
#include "cli/gog.hpp"

#include "loader/offsets.hpp"

#include "setup/data.hpp"
#include "setup/directory.hpp"
#include "setup/expression.hpp"
#include "setup/file.hpp"
#include "setup/info.hpp"
#include "setup/language.hpp"

#include "stream/chunk.hpp"
#include "stream/file.hpp"
#include "stream/slice.hpp"

#include "util/boostfs_compat.hpp"
#include "util/console.hpp"
#include "util/fstream.hpp"
#include "util/load.hpp"
#include "util/log.hpp"
#include "util/output.hpp"
#include "util/time.hpp"

namespace fs = boost::filesystem;

namespace {

static size_t probe_bin_files(const extract_options & o, const setup::info & info,
                              const fs::path & dir, const std::string & basename,
                              size_t format = 0, size_t start = 0) {
	
	size_t count = 0;
	
	std::vector<fs::path> files;
	
	for(size_t i = start;; i++) {
		
		fs::path file;
		if(format == 0) {
			file = dir / basename;
		} else {
			file = dir / stream::slice_reader::slice_filename(basename, i, format);
		}
		
		try {
			if(!fs::is_regular_file(file)) {
				break;
			}
		} catch(...) {
			break;
		}
		
		if(o.gog) {
			files.push_back(file);
		} else {
			log_warning << file.filename() << " is not part of the installer!";
			count++;
		}
		
		if(format == 0) {
			break;
		}
		
	}
	
	if(!files.empty()) {
		gog::process_bin_files(files, o, info);
	}
	
	return count;
}

struct file_output {
	
	fs::path name;
	util::ofstream stream;
	
	explicit file_output(const fs::path & file, boost::uint64_t offset = 0) : name(file) {
		try {
			std::ios_base::openmode mode = std::ios_base::out | std::ios_base::binary;
			if(offset == 0) mode |= std::ios_base::trunc;
			else mode |= std::ios_base::app;
			stream.open(name, mode);
			if(!stream.is_open()) {
				throw 0;
			}
		} catch(...) {
			throw std::runtime_error("Coul not open output file \"" + name.string() + '"');
		}
	}
	
};


struct gx_file {
	std::string path;

	unsigned long num_parts;
	boost::uint64_t total_size;
	std::string checksum;
};


struct gx_chunk {
	std::string checksum;
	boost::uint64_t size_packed, size;

	std::string lang, bitness, winver;

	boost::uint64_t offset;
	struct gx_file *file;
};

template <typename Entry>
class processed_item {
	
	std::string path_;
	const Entry * entry_;
	bool implied_;
public: //FIXME
	struct gx_chunk *gx_;

public:
	
	processed_item() : entry_(NULL), implied_(false), gx_(NULL) { }
	
	processed_item(const Entry * entry, const std::string & path, struct gx_chunk *gx = NULL, bool implied = false)
		: path_(path), entry_(entry), implied_(implied), gx_(gx) { }
	
	processed_item(const std::string & path, bool implied = false)
		: path_(path), entry_(NULL), implied_(implied), gx_(NULL) { }
		
	processed_item(const processed_item & o)
		: path_(o.path_), entry_(o.entry_), implied_(o.implied_), gx_(o.gx_) { }
	
	bool has_entry() const { return entry_ != NULL; }
	const Entry & entry() const { return *entry_; }
	const std::string & path() const { return path_; }
	boost::uint64_t offset() const { return gx_ ? gx_->offset : 0; }
	bool implied() const { return implied_; }
	bool has_gxchunk() const { return gx_ != NULL; }
	const struct gx_chunk& gxchunk() const { return *gx_;}

	void set_entry(const Entry * entry) { entry_ = entry; }
	void set_path(const std::string & path) { path_ = path; }
	void set_implied(bool implied) { implied_ = implied; }
	void set_gxchunk(struct gx_chunk *gx) { gx_ = gx; }
};

typedef processed_item<setup::file_entry> processed_file;
typedef processed_item<setup::directory_entry> processed_directory;

class path_filter {
	
	typedef std::pair<bool, std::string> Filter;
	std::vector<Filter> includes;
	
public:
	
	explicit path_filter(const extract_options & o) {
		BOOST_FOREACH(const std::string & include, o.include) {
			if(!include.empty() && include[0] == setup::path_sep) {
				includes.push_back(Filter(true, boost::to_lower_copy(include) + setup::path_sep));
			} else {
				includes.push_back(Filter(false, setup::path_sep + boost::to_lower_copy(include)
				                                 + setup::path_sep));
			}
		}
	}
	
	bool match(const std::string & path) const {
		
		if(includes.empty()) {
			return true;
		}
		
		BOOST_FOREACH(const Filter & i, includes) {
			if(i.first) {
				if(!i.second.compare(1, i.second.size() - 1,
				                     path + setup::path_sep, 0, i.second.size() - 1)) {
					return true;
				}
			} else {
				if((setup::path_sep + path + setup::path_sep).find(i.second) != std::string::npos) {
					return true;
				}
			}
		}
		
		return false;
	}
	
};

static void print_filter_info(const setup::item & item, bool temp) {
	
	bool first = true;

	if(!item.languages.empty()) {
		std::cout << (first ? " [" : ", ");
		first = false;
		std::cout << color::green << item.languages << color::reset;
	}
	
	if(temp) {
		std::cout << (first ? " [" : ", ");
		first = false;
		std::cout << color::cyan << "temp" << color::reset;
		
	}

	if(!first) {
		std::cout << "]";
	}
}

// constraint: keyA#keyB#keyC#   or !keyF#
static bool check_constraint(const std::string &key, const std::string &constraint)
{
	if(constraint.empty()) return true;
	if(key.compare("*")==0) return true;

	unsigned long p = 0;

	while(p < constraint.size())
	{
		bool valid = true;
		if(constraint[p] == '!') {
			valid = false;
			p++;
		}
		unsigned long e = constraint.find('#',p);

		if(e == std::string::npos) e = constraint.size();
		if( (constraint.compare(p, e-p, key) == 0) == valid ) return true;

		p = e+1;
	}
	return false;
}


static std::string parse_quoted(const std::string s, long unsigned int pos, long unsigned int len)
{

	const std::string sub = s.substr(pos, len);
	std::string::size_type st = sub.find('\'');
	if(st != std::string::npos) {
		st++;
		std::string::size_type end = sub.find('\'', st);
		if(end != std::string::npos) {
			end--;
			return sub.substr(st, end-st+1);
		}
	}
	    throw 0;
}


static long unsigned parse_uint(const std::string s, long unsigned int pos, long unsigned int len)
{
	return std::stoul( s.substr(pos, len) );
}


static long unsigned int next_arg(const std::string &s, const long unsigned int pos)
{
	std::string::size_type n = s.find(',', pos);
	if(n == std::string::npos)  n = s.find(')', pos);
	if(n == std::string::npos) {
		std::cout << "oops";
		throw 0;
	}
	return n;
}


static struct gx_chunk* get_gog_info(const setup::file_entry &item)
{
	static const char before[] = "before_install(";
	static const char after[] = "after_install(";
	static const char check[] = "check_if_install(";

	struct gx_chunk *ch = NULL;

	if( item.after_install.compare(0, sizeof(after)-1, after) == 0) {
		ch = new gx_chunk();
	        ch->offset = 0;
		ch->file = NULL;

	        long unsigned int st = sizeof(after)-1;
		long unsigned int end = next_arg(item.after_install, st);
	        ch->checksum = parse_quoted(item.after_install, st, end-st+1);

	        st = end + 1;
		end = next_arg(item.after_install, st);
	        ch->size_packed = parse_uint(item.after_install, st, end-st+1);

		st = end + 1;
	        end = next_arg(item.after_install, st);
		ch->size = parse_uint(item.after_install, st, end-st+1);

	//        std::cout << "parse:"<<item.after_install <<":"<<ch->checksum<<":"<<ch->size_packed<<":"<<ch->size<<"\n";
	} else if( !item.after_install.empty())
	        std::cout << "unhandled after install action: "<< item.after_install << "\n";


	if( item.before_install.compare(0, sizeof(before)-1, before) == 0) {
		if(!ch) {
			std::cout << "missing after_install definition:\n";
			return NULL;
		}

	        struct gx_file *file = new gx_file();

		long unsigned int ss = sizeof(before)-1;
	        long unsigned int end = next_arg(item.before_install, ss);
		file->checksum = parse_quoted(item.before_install, ss, end-ss+1);

	        ss = end + 1;
		end = next_arg(item.before_install, ss);
	        file->path = parse_quoted(item.before_install, ss, end-ss+1);

		ss = end + 1;
	        end = next_arg(item.before_install, ss);
		file->num_parts = parse_uint(item.before_install, ss, end-ss+1);

	        ch->file = file;
		file->total_size = ch->size;
	} else if( !item.before_install.empty() )
	        std::cout << "unhandled before install action(" << item.before_install << "):" << item.destination << "\n";

	if( item.check.compare(0, sizeof(check)-1, check) == 0) {
		if(!ch) {
	            std::cout << "missing gog data description:"<<item.before_install<<item.after_install<<item.check<<"\n";
		    return NULL;
	        }
		long unsigned int ss = sizeof(check)-1;
	        long unsigned int end = next_arg(item.check, ss);
		ch->lang = parse_quoted(item.check, ss, end-ss+1);


	        ss = end + 1;
		end = next_arg(item.check, ss);
	        ch->bitness = parse_quoted(item.check, ss, end-ss+1);

		ss = end + 1;
	        end = next_arg(item.check, ss);
		ch->winver = parse_quoted(item.check, ss, end-ss+1);
	} else if( !item.check.empty() )
	        std::cout << "unhandled check install action(" << item.check << "):" << item.destination << "\n";

	return ch;
}


static void print_filter_info(const setup::file_entry & file) {
	bool is_temp = !!(file.options & setup::file_entry::DeleteAfterInstall);
	print_filter_info(file, is_temp);
}

static void print_filter_info(const setup::directory_entry & dir) {
	bool is_temp = !!(dir.options & setup::directory_entry::DeleteAfterInstall);
	print_filter_info(dir, is_temp);
}

static void print_size_info(const stream::file & file, boost::uint64_t packed=0, boost::uint64_t size=0) {
	
	if(logger::debug) {
		std::cout << " @ " << print_hex(file.offset);
	}
	
	if(packed && size)
		std::cout << " (" << color::dim_cyan << print_bytes(packed) << color::reset << " => " << color::dim_cyan << print_bytes(size) <<color::reset << ") ";
	else
		std::cout << " (" << color::dim_cyan << print_bytes(file.size) << color::reset << ")";

}


static void print_gx_info(struct gx_chunk *gx)
{
	if(!gx) return;

	if(!gx->lang.empty())
		std::cout << " {"<<gx->lang<<"} ";
	if(!gx->bitness.empty() && (gx->bitness.compare("32#64#")!=0) )
		std::cout << " {"<<gx->bitness<<"} ";
	if(!gx->winver.empty())
		std::cout << " {"<<gx->winver<<"}";
}

static bool prompt_overwrite() {
	return true; // TODO the user always overwrites
}

static const char * handle_collision(const setup::file_entry & oldfile,
                                     const setup::data_entry & olddata,
                                     const setup::file_entry & newfile,
                                     const setup::data_entry & newdata) {
	
	bool allow_timestamp = true;
	
	if(!(newfile.options & setup::file_entry::IgnoreVersion)) {
		
		bool version_info_valid = !!(newdata.options & setup::data_entry::VersionInfoValid);
		
		if(olddata.options & setup::data_entry::VersionInfoValid) {
			allow_timestamp = false;
			
			if(!version_info_valid || olddata.file_version > newdata.file_version) {
				if(!(newfile.options & setup::file_entry::PromptIfOlder) || !prompt_overwrite()) {
					return "old version";
				}
			} else if(newdata.file_version == olddata.file_version
				   && !(newfile.options & setup::file_entry::OverwriteSameVersion)) {
				
				if((newfile.options & setup::file_entry::ReplaceSameVersionIfContentsDiffer)
				   && olddata.file.checksum == newdata.file.checksum) {
					return "duplicate (checksum)";
				}
				
				if(!(newfile.options & setup::file_entry::CompareTimeStamp)) {
					return "duplicate (version)";
				}
				
				allow_timestamp = true;
			}
			
		} else if(version_info_valid) {
			allow_timestamp = false;
		}
		
	}
	
	if(allow_timestamp && (newfile.options & setup::file_entry::CompareTimeStamp)) {
		
		if(newdata.timestamp == olddata.timestamp
		   && newdata.timestamp_nsec == olddata.timestamp_nsec) {
			return "duplicate (modification time)";
		}
		
		
		if(newdata.timestamp < olddata.timestamp
		   || (newdata.timestamp == olddata.timestamp
		       && newdata.timestamp_nsec < olddata.timestamp_nsec)) {
			if(!(newfile.options & setup::file_entry::PromptIfOlder) || !prompt_overwrite()) {
				return "old version (modification time)";
			}
		}
		
	}
	
	if((newfile.options & setup::file_entry::ConfirmOverwrite) && !prompt_overwrite()) {
		return "user chose not to overwrite";
	}
	
	if(oldfile.attributes != boost::uint32_t(-1)
	   && (oldfile.attributes & setup::file_entry::ReadOnly) != 0) {
		if(!(newfile.options & setup::file_entry::OverwriteReadOnly) && !prompt_overwrite()) {
			return "user chose not to overwrite read-only file";
		}
	}
	
	return NULL; // overwrite old file
}

typedef boost::unordered_map<std::string, processed_file> FilesMap;
#if BOOST_VERSION >= 104800
typedef boost::container::flat_map<std::string, processed_directory> DirectoriesMap;
#else
typedef std::map<std::string, processed_directory> DirectoriesMap;
#endif
typedef boost::unordered_map<std::string, std::vector<processed_file> > CollisionMap;

static std::string parent_dir(const std::string & path) {
	
	size_t pos = path.find_last_of(setup::path_sep);
	if(pos == std::string::npos) {
		return std::string();
	}
	
	return path.substr(0, pos);
}

static bool insert_dirs(DirectoriesMap & processed_directories, const path_filter & includes,
                        const std::string & internal_path, std::string & path, bool implied) {
	
	std::string dir = parent_dir(path);
	std::string internal_dir = parent_dir(internal_path);
	
	if(internal_dir.empty()) {
		return false;
	}
	
	if(implied || includes.match(internal_dir)) {
		
		std::pair<DirectoriesMap::iterator, bool> existing = processed_directories.insert(
			std::make_pair(internal_dir, processed_directory(dir))
		);
		
		if(implied) {
			existing.first->second.set_implied(true);
		}
		
		if(!existing.second) {
			if(existing.first->second.path() != dir) {
				// Existing dir case differs, fix path
				if(existing.first->second.path().length() == dir.length()) {
					path.replace(0, dir.length(), existing.first->second.path());
				} else {
					path = existing.first->second.path() + path.substr(dir.length());
				}
				return true;
			} else {
				return false;
			}
		}
		
		implied = true;
	}
	
	size_t oldlength = dir.length();
	if(insert_dirs(processed_directories, includes, internal_dir, dir, implied)) {
		// Existing dir case differs, fix path
		if(dir.length() == oldlength) {
			path.replace(0, dir.length(), dir);
		} else {
			path = dir + path.substr(oldlength);
		}
		// Also fix previously inserted directory
		DirectoriesMap::iterator inserted = processed_directories.find(internal_dir);
		if(inserted != processed_directories.end()) {
			inserted->second.set_path(dir);
		}
		return true;
	}
	
	return false;
}

static bool rename_collision(const extract_options & o, FilesMap & processed_files,
                             const std::string & path, const processed_file & other,
                             bool common_component, bool common_language, bool first) {
	
	const setup::file_entry & file = other.entry();
	
	bool require_number_suffix = !first || (o.collisions == RenameAllCollisions);
	std::ostringstream oss;
	
	if(!common_component && !file.components.empty()) {
		if(setup::is_simple_expression(file.components)) {
			require_number_suffix = false;
			oss << '#' << file.components;
		}
	}
	if(!common_language && !file.languages.empty()) {
		if(setup::is_simple_expression(file.languages)) {
			require_number_suffix = false;
			if(file.languages != o.default_language) {
				oss << '@' << file.languages;
			}
		}
	}
	
	size_t i = 0;
	std::string suffix = oss.str();
	if(require_number_suffix) {
		oss << '$' << i++;
	}
	for(;;) {
		std::pair<FilesMap::iterator, bool> insertion = processed_files.insert(std::make_pair(
			path + oss.str(), processed_file(&file, other.path() + oss.str())
		));
		if(insertion.second) {
			// Found an available name and inserted
			return true;
		}
		if(&insertion.first->second.entry() == &file) {
			// File already has the desired name, abort
			return false;
		}
		oss.str(suffix);
		oss << '$' << i++;
	}
	
}

static void rename_collisions(const extract_options & o, FilesMap & processed_files,
                              const CollisionMap & collisions) {
	
	BOOST_FOREACH(const CollisionMap::value_type & collision, collisions) {
		
		const std::string & path = collision.first;
		
		const processed_file & base = processed_files[path];
		const setup::file_entry & file = base.entry();
		
		bool common_component = true;
		bool common_language = true;
		BOOST_FOREACH(const processed_file & other, collision.second) {
			common_component = common_component && other.entry().components == file.components;
			common_language = common_language && other.entry().languages == file.languages;
		}
		
		bool ignore_component = common_component || o.collisions != RenameAllCollisions;
		if(rename_collision(o, processed_files, path, base,
		                    ignore_component, common_language, true)) {
			processed_files.erase(path);
		}
		
		BOOST_FOREACH(const processed_file & other, collision.second) {
			rename_collision(o, processed_files, path, other,
			                 common_component, common_language, false);
		}
		
	}
}

} // anonymous namespace

void process_file(const fs::path & file, const extract_options & o) {
	
	bool is_directory;
	try {
		is_directory = fs::is_directory(file);
	} catch(...) {
		throw std::runtime_error("Could not open file \"" + file.string()
		                         + "\": access denied");
	}
	if(is_directory) {
		throw std::runtime_error("Input file \"" + file.string() + "\" is a directory!");
	}
	
	util::ifstream ifs;
	try {
		ifs.open(file, std::ios_base::in | std::ios_base::binary);
		if(!ifs.is_open()) {
			throw 0;
		}
	} catch(...) {
		throw std::runtime_error("Could not open file \"" + file.string() + '"');
	}
	
	loader::offsets offsets;
	offsets.load(ifs);
	
#ifdef DEBUG
	if(logger::debug) {
		print_offsets(offsets);
		std::cout << '\n';
	}
#endif
	
	setup::info::entry_types entries = 0;
	if(o.list || o.test || o.extract) {
		entries |= setup::info::Files;
		entries |= setup::info::Directories;
		entries |= setup::info::DataEntries;
	}
	if(o.list_languages) {
		entries |= setup::info::Languages;
	}
	if(o.gog_game_id || o.gog) {
		entries |= setup::info::RegistryEntries;
	}
#ifdef DEBUG
	if(logger::debug) {
		entries = setup::info::entry_types::all();
	}
#endif
	
	ifs.seekg(offsets.header_offset);
	setup::info info;
	try {
		info.load(ifs, entries);
	} catch(const std::ios_base::failure & e) {
		std::ostringstream oss;
		oss << "Stream error while parsing setup headers!\n";
		oss << " ├─ detected setup version: " << info.version << '\n';
		oss << " └─ error reason: " << e.what();
		throw format_error(oss.str());
	}
	
	if(!o.quiet) {
		const std::string & name = info.header.app_versioned_name.empty()
		                           ? info.header.app_name : info.header.app_versioned_name;
		const char * verb = "Inspecting";
		if(o.extract) {
			verb = "Extracting";
		} else if(o.test) {
			verb = "Testing";
		} else if(o.list) {
			verb = "Listing";
		}
		std::cout << verb << " \"" << color::green << name << color::reset
		          << "\" - setup data version " << color::white << info.version << color::reset
		          << std::endl;
	}
	
#ifdef DEBUG
	if(logger::debug) {
		std::cout << '\n';
		print_info(info);
		std::cout << '\n';
	}
#endif
	
	bool multiple_sections = (o.list_languages + o.gog_game_id + o.list > 1);
	if(!o.quiet && multiple_sections) {
		std::cout << '\n';
	}
	
	if(o.list_languages) {
		if(o.silent) {
			BOOST_FOREACH(const setup::language_entry & language, info.languages) {
				std::cout << language.name <<' ' << language.language_name << '\n';
			}
		} else {
			if(multiple_sections) {
				std::cout << "Languages:\n";
			}
			BOOST_FOREACH(const setup::language_entry & language, info.languages) {
				std::cout << " - " << color::green << language.name << color::reset;
				std::cout << ": " << color::white << language.language_name << color::reset << '\n';
			}
			if(info.languages.empty()) {
				std::cout << " (none)\n";
			}
		}
		if((o.silent || !o.quiet) && multiple_sections) {
			std::cout << '\n';
		}
	}
	
	if(o.gog_game_id) {
		std::string id = gog::get_game_id(info);
		if(id.empty()) {
			if(!o.quiet) {
				std::cout << "No GOG.com game ID found!\n";
			}
		} else if(!o.silent) {
			std::cout << "GOG.com game ID is " << color::cyan << id << color::reset << '\n';
		} else {
			std::cout << id;
		}
		if((o.silent || !o.quiet) && multiple_sections) {
			std::cout << '\n';
		}
	}
	
	if(!o.list && !o.test && !o.extract) {
		return;
	}
	
	if(!o.silent && multiple_sections) {
		std::cout << "Files:\n";
	}
	
	FilesMap processed_files;
	std::list<processed_file> dummy_files;
	#if BOOST_VERSION >= 105000
	processed_files.reserve(info.files.size());
	#endif
	
	DirectoriesMap processed_directories;
	#if BOOST_VERSION >= 104800
	processed_directories.reserve(info.directories.size()
	                              + size_t(std::log(double(info.files.size()))));
	#endif
	
	CollisionMap collisions;
	
	path_filter includes(o);
	
	// Filter the directories to be created
	BOOST_FOREACH(const setup::directory_entry & directory, info.directories) {
		
		if(!o.extract_temp && (directory.options & setup::directory_entry::DeleteAfterInstall)) {
			continue; // Ignore temporary dirs
		}
		
		if(!directory.languages.empty()) {
			if(!o.language.empty() && !setup::expression_match(o.language, directory.languages)) {
				continue; // Ignore other languages
			}
		} else if(o.language_only) {
			continue; // Ignore language-agnostic dirs
		}
	        //std::cout << "dir:"<< directory.name<<":"<<directory.check<<":"<<directory.before_install<<"\n";
		std::string path = o.filenames.convert(directory.name);
		if(path.empty()) {
			continue; // Don't know what to do with this
		}
		std::string internal_path = boost::algorithm::to_lower_copy(path);
		
		bool path_included = includes.match(internal_path);
		
		insert_dirs(processed_directories, includes, internal_path, path, path_included);
		
		DirectoriesMap::iterator it;
		if(path_included) {
			std::pair<DirectoriesMap::iterator, bool> existing = processed_directories.insert(
				std::make_pair(internal_path, processed_directory(path))
			);
			it = existing.first;
		} else {
			it = processed_directories.find(internal_path);
			if(it == processed_directories.end()) {
				continue;
			}
		}
		
		it->second.set_entry(&directory);
	}
	

	unsigned long rem_parts = 0;
	struct gx_file *gxfile = NULL;

	// Filter the files to be extracted
	BOOST_FOREACH(const setup::file_entry & file, info.files) {
		
		if(file.location >= info.data_entries.size()) {
			continue; // Ignore external files (copy commands)
		}
		
		if(!o.extract_temp && (file.options & setup::file_entry::DeleteAfterInstall)) {
			continue; // Ignore temporary files
		}
		
		if(!file.languages.empty()) {
			if(!o.language.empty() && !setup::expression_match(o.language, file.languages)) {
				continue; // Ignore other languages
			}
		} else if(o.language_only) {
			continue; // Ignore language-agnostic files
		}
		
		std::string path = o.filenames.convert(file.destination);
		if(path.empty()) {
			continue; // Internal file, not extracted
		}

		struct gx_chunk *ch = NULL;
		if( o.gog ) ch = get_gog_info(file);

		if(ch) {
			if(!ch->file) {
				if(!gxfile || (rem_parts < 1) ) {
					std::cout << "oops\n";
					continue;
				}
				ch->offset = gxfile->total_size;
				ch->file = gxfile;
				gxfile->total_size += ch->size;
				rem_parts--;
				if(rem_parts == 0) gxfile=NULL;
			} else {
				if(ch->file->num_parts > 1) {
					if(rem_parts > 0) {
						std::cout << "oopsie\n";
					}
					gxfile = ch->file;
					rem_parts = ch->file->num_parts - 1;
				}
			}

			/*std::cout << "f:"<<file.destination<<" => "<<ch->file->path<<"@"<<ch->offset<<"\n";
			std::cout << "lang:"<<ch->lang<<"="<<check_constraint("en-US", ch->lang)<<" bit:"<<ch->bitness<<"="<<check_constraint("64",ch->bitness)<<" winver:"<<ch->winver<<"="<<check_constraint("win7", ch->winver)<<"\n";*/

			if( !check_constraint(o.gog_lang, ch->lang)
			   || !check_constraint(o.gog_osbits, ch->bitness)
			   || !check_constraint(o.gog_winver, ch->winver) ) {
//				std::cout << "skpped\n";
	                        continue;
		        }
			path = o.filenames.convert(ch->file->path);
		}

		std::string internal_path = boost::algorithm::to_lower_copy(path);

		bool path_included = includes.match(internal_path);
		insert_dirs(processed_directories, includes, internal_path, path, path_included);
		if(!path_included) {
		    continue; // Ignore excluded file
		}

		std::pair<FilesMap::iterator, bool> insertion = processed_files.insert(std::make_pair(
			internal_path, processed_file(&file, path, ch)
		));
		
		if(!insertion.second) {
			// Collision!
			processed_file & existing = insertion.first->second;

			if( existing.has_gxchunk() && (existing.gxchunk().file == ch->file) ) {
				// no collision handling
				dummy_files.push_back( processed_file(&file, path, ch) );
				continue;
			}

			if(o.collisions == ErrorOnCollisions) {
				throw std::runtime_error("Collision: " + path);
			} else if(o.collisions == RenameAllCollisions) {
				collisions[internal_path].push_back(processed_file(&file, path));
			} else {
				
				const setup::data_entry & newdata = info.data_entries[file.location];
				const setup::data_entry & olddata = info.data_entries[existing.entry().location];
				const char * skip = handle_collision(existing.entry(), olddata, file, newdata);
				
				if(!o.default_language.empty()) {
					bool oldlang = setup::expression_match(o.default_language, file.languages);
					bool newlang = setup::expression_match(o.default_language, existing.entry().languages);
					if(oldlang && !newlang) {
						skip = NULL;
					} else if(!oldlang && newlang) {
						skip = "overwritten";
					}
				}
				
				if(o.collisions == RenameCollisions) {
					const setup::file_entry & clobberedfile = skip ? file : existing.entry();
					const std::string & clobberedpath = skip ? path : existing.path();
					collisions[internal_path].push_back(processed_file(&clobberedfile, clobberedpath));
				} else if(!o.silent) {
					std::cout << " - ";
					const std::string & clobberedpath = skip ? path : existing.path();
					std::cout << '"' << color::dim_yellow << clobberedpath << color::reset << '"';
					print_filter_info(skip ? file : existing.entry());
					if(!o.quiet) {
						print_gx_info(skip ? existing.gx_ : ch);
						print_size_info(skip ? newdata.file : olddata.file);
					}
					std::cout << " - " << (skip ? skip : "overwritten") << '\n';
				}
				
				if(!skip) {
					existing.set_entry(&file);
					if(file.type != setup::file_entry::UninstExe) {
						// Old file is "deleted" first → use case from new file
						existing.set_path(path);
					}
				}
				
			}
			
		}
		
	}
	
	if(o.collisions == RenameCollisions || o.collisions == RenameAllCollisions) {
		rename_collisions(o, processed_files, collisions);
		collisions.clear();
	}
	
	
	if(o.extract && !o.output_dir.empty()) {
		fs::create_directories(o.output_dir);
	}
	
	if(o.list || o.extract) {
		
		BOOST_FOREACH(const DirectoriesMap::value_type & i, processed_directories) {
			
			const std::string & path = i.second.path();
			
			if(o.list && !i.second.implied()) {
				
				if(!o.silent) {
					
					std::cout << " - ";
					std::cout << '"' << color::dim_white << path << setup::path_sep << color::reset << '"';
					if(i.second.has_entry()) {
						print_filter_info(i.second.entry());
					}
					std::cout << '\n';
					
				} else {
					std::cout << color::dim_white << path << setup::path_sep << color::reset << '\n';
				}
				
			}
			
			if(o.extract) {
				fs::path dir = o.output_dir / path;
				try {
					fs::create_directory(dir);
				} catch(...) {
					throw std::runtime_error("Could not create directory \"" + dir.string() + '"');
				}
			}
			
		}
		
	}
	
	std::vector< std::vector<const processed_file *> > files_for_location;
	files_for_location.resize(info.data_entries.size());
	BOOST_FOREACH(const FilesMap::value_type & i, processed_files) {
		const processed_file & file = i.second;
		files_for_location[file.entry().location].push_back(&file);
	}

	BOOST_FOREACH(const processed_file &file, dummy_files) {
		files_for_location[file.entry().location].push_back(&file);
	}
	
	boost::uint64_t total_size = 0;
	size_t max_slice = 0;
	
	typedef std::map<stream::file, size_t> Files;
	typedef std::map<stream::chunk, Files> Chunks;
	Chunks chunks;
	for(size_t i = 0; i < info.data_entries.size(); i++) {
		setup::data_entry & location = info.data_entries[i];
		if(!offsets.data_offset) {
			max_slice = std::max(max_slice, location.chunk.first_slice);
			max_slice = std::max(max_slice, location.chunk.last_slice);
		}
		if(files_for_location[i].empty()) {
			continue;
		}
		if(location.chunk.compression == stream::UnknownCompression) {
			location.chunk.compression = info.header.compression;
		}
		chunks[location.chunk][location.file] = i;
		total_size += location.file.size;
	}
	
	fs::path dir = file.parent_path();
	std::string basename = util::as_string(file.stem());
	
	boost::scoped_ptr<stream::slice_reader> slice_reader;
	if(o.extract || o.test) {
		if(offsets.data_offset) {
			slice_reader.reset(new stream::slice_reader(&ifs, offsets.data_offset));
		} else {
			slice_reader.reset(new stream::slice_reader(dir, basename, info.header.slices_per_disk));
		}
	}
	
	progress extract_progress(total_size);

	BOOST_FOREACH(const Chunks::value_type & chunk, chunks) {
		
		debug("[starting " << chunk.first.compression << " chunk @ slice " << chunk.first.first_slice
		      << " + " << print_hex(offsets.data_offset) << " + " << print_hex(chunk.first.offset)
		      << ']');
	        //std::cout << "[chunk:] " << chunk.first.compression << "\n";
		if(chunk.first.encrypted) {
			log_warning << "Skipping encrypted chunk (unsupported)";
		}
		
		stream::chunk_reader::pointer chunk_source;
		if((o.extract || o.test) && !chunk.first.encrypted) {
			chunk_source = stream::chunk_reader::get(*slice_reader, chunk.first);
		}
		boost::uint64_t offset = 0;
		
		BOOST_FOREACH(const Files::value_type & location, chunk.second) {
			const stream::file & file = location.first;
			const std::vector<const processed_file *> & names = files_for_location[location.second];
			
			if(file.offset > offset) {
				debug("discarding " << print_bytes(file.offset - offset)
				      << " @ " << print_hex(offset));
				if(chunk_source.get()) {
					util::discard(*chunk_source, file.offset - offset);
				}
			}
			
			// Print filename and size
			if(o.list) {
				
				extract_progress.clear(DeferredClear);
				
				if(!o.silent) {
					
					std::cout << " - ";
					bool named = false;
					boost::uint64_t packed=0,size=0;
					BOOST_FOREACH(const processed_file * name, names) {
						if(named) {
							std::cout << ", ";
						}
						if(name->has_gxchunk()) {
							if(name->offset() > 0) std::cout << "++";
							size = name->gxchunk().size;
							packed = name->gxchunk().size_packed;
						}
						if(chunk.first.encrypted) {
							std::cout << '"' << color::dim_yellow << name->path() << color::reset << '"';
						} else {
							std::cout << '"' << color::white << name->path() << color::reset << '"';
						}
						print_filter_info(name->entry());
						print_gx_info(name->gx_);
						named = true;
					}
					if(!named) {
						std::cout << color::white << "unnamed file" << color::reset;
					}

					if(!o.quiet) {
						print_size_info(file, packed,size);
					}
					if(chunk.first.encrypted) {
						std::cout << " - encrypted";
					}
					std::cout << '\n';
					
				} else {
					BOOST_FOREACH(const processed_file * name, names) {
						std::cout << color::white << name->path() << color::reset << '\n';
					}
				}
				
				bool updated = extract_progress.update(0, true);
				if(!updated && (o.extract || o.test)) {
					std::cout.flush();
				}
				
			}
			
			// Seek to the correct position within the chunk
			if(chunk_source.get() && file.offset < offset) {
				std::ostringstream oss;
				oss << "Bad offset while extracting files: file start (" << file.offset
				    << ") is before end of previous file (" << offset << ")!";
				throw format_error(oss.str());
			}
			offset = file.offset + file.size;
			
			if(!chunk_source.get()) {
				continue; // Not extracting/testing this file
			}
			
			crypto::checksum checksum;
			bool packed = false;
			BOOST_FOREACH(const processed_file * name, names) {
				if( name->has_gxchunk() ) packed = true;
			}

			// Open input file
			stream::file_reader::pointer file_source;
			file_source = stream::file_reader::get(*chunk_source, file, &checksum, packed);
			
			// Open output files
			boost::ptr_vector<file_output> output;
			if(!o.test) {
				output.reserve(names.size());
				BOOST_FOREACH(const processed_file * name, names) {
					try {
						//std::cout << "out:"<<name->path()<<"@"<<name->offset()<<"\n";
						output.push_back(new file_output(o.output_dir / name->path(), name->offset()));
					} catch(boost::bad_pointer &) {
						// should never happen
						std::terminate();
					}
				}
			}
			
			// Copy data
			while(!file_source->eof()) {
				char buffer[8192 * 10];
				std::streamsize buffer_size = std::streamsize(boost::size(buffer));
				std::streamsize n = file_source->read(buffer, buffer_size).gcount();
				if(n > 0) {
					BOOST_FOREACH(file_output & out, output) {
						out.stream.write(buffer, n);
						if(out.stream.fail()) {
							throw std::runtime_error("Error writing file \""
							                         + out.name.string() + '"');
						}
					}
					extract_progress.update(boost::uint64_t(n));
				}
			}
			
			// Adjust file timestamps
			if(o.preserve_file_times) {
				const setup::data_entry & data = info.data_entries[location.second];
				util::time filetime = data.timestamp;
				if(o.local_timestamps && !(data.options & data.TimeStampInUTC)) {
					filetime = util::to_local_time(filetime);
				}
				BOOST_FOREACH(file_output & out, output) {
					out.stream.close();
					if(!util::set_file_time(out.name, filetime, data.timestamp_nsec)) {
						log_warning << "Error setting timestamp on file " << out.name;
					}
				}
			}
			
			// Verify checksums
			if(checksum != file.checksum) {
				log_warning << "Checksum mismatch:\n"
				            << " ├─ actual:   " << checksum << '\n'
				            << " └─ expected: " << file.checksum;
				if(o.test) {
					throw std::runtime_error("Integrity test failed!");
				}
			}
		}
		
		#ifdef DEBUG
		if(offset < chunk.first.size) {
			debug("discarding " << print_bytes(chunk.first.size - offset)
			      << " at end of chunk @ " << print_hex(offset));
		}
		#endif
	}
	
	extract_progress.clear();
	
	if(o.warn_unused || o.gog) {
		size_t bin_count = 0;
		bin_count += size_t(probe_bin_files(o, info, dir, basename + ".bin"));
		bin_count += size_t(probe_bin_files(o, info, dir, basename + "-0" + ".bin"));
		size_t slice =  0;
		size_t format = 1;
		if(!offsets.data_offset && info.header.slices_per_disk == 1) {
			slice = max_slice + 1;
		}
		bin_count += probe_bin_files(o, info, dir, basename, format, slice);
		slice = 0;
		format = 2;
		if(!offsets.data_offset && info.header.slices_per_disk != 1) {
			slice = max_slice + 1;
			format = info.header.slices_per_disk;
		}
		bin_count += probe_bin_files(o, info, dir, basename, format, slice);
		if(bin_count) {
			const char * verb = "inspecting";
			if(o.extract) {
				verb = "extracting";
			} else if(o.test) {
				verb = "testing";
			} else if(o.list) {
				verb = "listing the contents of";
			}
			std::cerr << color::yellow << "Use the --gog option to try " << verb << " "
			          << (bin_count > 1 ? "these files" : "this file") << ".\n" << color::reset;
		}
	}
	
}
