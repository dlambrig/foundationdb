/*
 * serialize.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once
#include "flow/Error.h"
#include "flow/Arena.h"
#include "flow/flat_buffers.h"
#include "flow/ProtocolVersion.h"

template <class Ar>
struct LoadContext {
	Ar& ar;
	LoadContext(Ar& ar) : ar(ar) {}
	Arena& arena() { return ar.arena(); }

	const uint8_t* tryReadZeroCopy(const uint8_t* ptr, unsigned len) {
		if constexpr (Ar::ownsUnderlyingMemory) {
			return ptr;
		} else {
			if (len == 0) return nullptr;
			uint8_t* dat = new (arena()) uint8_t[len];
			std::copy(ptr, ptr + len, dat);
			return dat;
		}
	}

	void addArena(Arena& arena) { arena = ar.arena(); }
};

template <class ReaderImpl>
class _ObjectReader {
	ProtocolVersion mProtocolVersion;
public:

	ProtocolVersion protocolVersion() const { return mProtocolVersion; }
	void setProtocolVersion(ProtocolVersion v) { mProtocolVersion = v; }

	template <class... Items>
	void deserialize(FileIdentifier file_identifier, Items&... items) {
		const uint8_t* data = static_cast<ReaderImpl*>(this)->data();
		LoadContext<ReaderImpl> context(*static_cast<ReaderImpl*>(this));
		ASSERT(read_file_identifier(data) == file_identifier);
		load_members(data, context, items...);
	}

	template <class Item>
	void deserialize(Item& item) {
		deserialize(FileIdentifierFor<Item>::value, item);
	}
};

class ObjectReader : public _ObjectReader<ObjectReader> {
	friend class _IncludeVersion;
	ObjectReader& operator>> (ProtocolVersion& version) {
		uint64_t result;
		memcpy(&result, _data, sizeof(result));
		_data += sizeof(result);
		return *this;
	}
public:
	static constexpr bool ownsUnderlyingMemory = false;

	template<class VersionOptions>
	ObjectReader(const uint8_t* data, VersionOptions vo) : _data(data) {
		vo.read(*this);
	}

	const uint8_t* data() { return _data; }

	Arena& arena() { return _arena; }

private:
	const uint8_t* _data;
	Arena _arena;
};

class ArenaObjectReader : public _ObjectReader<ArenaObjectReader> {
	friend class _IncludeVersion;
	ArenaObjectReader& operator>> (ProtocolVersion& version) {
		uint64_t result;
		memcpy(&result, _data, sizeof(result));
		_data += sizeof(result);
		return *this;
	}
public:
	static constexpr bool ownsUnderlyingMemory = true;

	template <class VersionOptions>
	ArenaObjectReader(Arena const& arena, const StringRef& input, VersionOptions vo)
	  : _data(input.begin()), _arena(arena) {
		vo.read(*this);
	}

	const uint8_t* data() { return _data; }

	Arena& arena() { return _arena; }

private:
	const uint8_t* _data;
	Arena _arena;
};

class ObjectWriter {
	friend class _IncludeVersion;
	bool writeProtocolVersion = false;
	ObjectWriter& operator<< (const ProtocolVersion& version) {
		writeProtocolVersion = true;
		return *this;
	}
	ProtocolVersion mProtocolVersion;
public:
	template <class VersionOptions>
	ObjectWriter(VersionOptions vo) {
		vo.write(*this);
	}
	template <class VersionOptions>
	explicit ObjectWriter(std::function<uint8_t*(size_t)> customAllocator, VersionOptions vo)
	  : customAllocator(customAllocator) {
		vo.write(*this);
	}
	template <class... Items>
	void serialize(FileIdentifier file_identifier, Items const&... items) {
		int allocations = 0;
		auto allocator = [this, &allocations](size_t size_) {
			++allocations;
			size = size_;
			auto toAllocate = writeProtocolVersion ? size + sizeof(uint64_t) : size;
			if (customAllocator) {
				data = customAllocator(toAllocate);
			} else {
				data = new (arena) uint8_t[toAllocate];
			}
			if (writeProtocolVersion) {
				auto v = protocolVersion().versionWithFlags();
				memcpy(data, &v, sizeof(uint64_t));
				return data + sizeof(uint64_t);
			}
			return data;
		};
		ASSERT(data == nullptr); // object serializer can only serialize one object
		save_members(allocator, file_identifier, items...);
		ASSERT(allocations == 1);
	}

	template <class Item>
	void serialize(Item const& item) {
		serialize(FileIdentifierFor<Item>::value, item);
	}

	StringRef toStringRef() const {
		return StringRef(data, size);
	}

	Standalone<StringRef> toString() const {
		ASSERT(!customAllocator);
		return Standalone<StringRef>(toStringRef(), arena);
	}

	template <class Item, class VersionOptions>
	static Standalone<StringRef> toValue(Item const& item, VersionOptions vo) {
		ObjectWriter writer(vo);
		writer.serialize(item);
		return writer.toString();
	}

	ProtocolVersion protocolVersion() const { return mProtocolVersion; }
	void setProtocolVersion(ProtocolVersion v) {
		mProtocolVersion = v;
		ASSERT(mProtocolVersion.isValid());
	}

private:
	Arena arena;
	std::function<uint8_t*(size_t)> customAllocator;
	uint8_t* data = nullptr;
	int size = 0;
};

// this special case is needed - the code expects
// Standalone<T> and T to be equivalent for serialization
namespace detail {

template <class T>
struct LoadSaveHelper<Standalone<T>> {
	template <class Context>
	void load(Standalone<T>& member, const uint8_t* current, Context& context) {
		helper.load(member.contents(), current, context);
		context.addArena(member.arena());
	}

	template <class Writer>
	RelativeOffset save(const Standalone<T>& member, Writer& writer, const VTableSet* vtables) {
		return helper.save(member.contents(), writer, vtables);
	}

private:
	LoadSaveHelper<T> helper;
};

} // namespace detail
