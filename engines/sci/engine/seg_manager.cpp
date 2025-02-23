/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "sci/sci.h"
#include "sci/engine/seg_manager.h"
#include "sci/engine/state.h"
#include "sci/engine/script.h"
#ifdef ENABLE_SCI32
#include "sci/engine/guest_additions.h"
#endif

namespace Sci {


SegManager::SegManager(ResourceManager *resMan, ScriptPatcher *scriptPatcher)
	: _resMan(resMan), _scriptPatcher(scriptPatcher) {
	_heap.push_back(0);

	_clonesSegId = 0;
	_listsSegId = 0;
	_nodesSegId = 0;
	_hunksSegId = 0;

	_saveDirPtr = NULL_REG;
	_parserPtr = NULL_REG;

#ifdef ENABLE_SCI32
	_arraysSegId = 0;
	_bitmapSegId = 0;
#endif

	createClassTable();
}

SegManager::~SegManager() {
	resetSegMan();
}

void SegManager::resetSegMan() {
	// Free memory
	for (uint i = 0; i < _heap.size(); i++) {
		if (_heap[i])
			deallocate(i);
	}

	_heap.clear();

	// And reinitialize
	_heap.push_back(0);

	_clonesSegId = 0;
	_listsSegId = 0;
	_nodesSegId = 0;
	_hunksSegId = 0;

#ifdef ENABLE_SCI32
	_arraysSegId = 0;
	_bitmapSegId = 0;
#endif

	// Reinitialize class table
	_classTable.clear();
	createClassTable();
}

void SegManager::initSysStrings() {
	if (getSciVersion() <= SCI_VERSION_1_1) {
		// We need to allocate system strings in one segment, for compatibility reasons
		allocDynmem(512, "system strings", &_saveDirPtr);
		_parserPtr = make_reg(_saveDirPtr.getSegment(), _saveDirPtr.getOffset() + 256);
#ifdef ENABLE_SCI32
	} else {
		SciArray *saveDirString = allocateArray(kArrayTypeString, 256, &_saveDirPtr);
		saveDirString->byteAt(0) = '\0';

		_parserPtr = NULL_REG;	// no SCI2 game had a parser
#endif
	}
}

SegmentId SegManager::findFreeSegment() const {
	// The following is a very crude approach: We find a free segment id by
	// scanning from the start. This can be slow if the number of segments
	// becomes large. Optimizations are possible and easy, but I'll refrain
	// from attempting any until we determine we actually need it.
	uint seg = 1;
	while (seg < _heap.size() && _heap[seg]) {
		++seg;
	}
	assert(seg < 65536);
	return seg;
}

SegmentObj *SegManager::allocSegment(SegmentObj *mem, SegmentId *segid) {
	// Find a free segment
	SegmentId id = findFreeSegment();
	if (segid)
		*segid = id;

	if (!mem)
		error("SegManager: invalid mobj");

	// ... and put it into the (formerly) free segment.
	if (id >= (int)_heap.size()) {
		assert(id == (int)_heap.size());
		_heap.push_back(0);
	}
	_heap[id] = mem;

	return mem;
}

Script *SegManager::allocateScript(int script_nr, SegmentId *segid) {
	// Check if the script already has an allocated segment. If it
	// does, return that segment.
	*segid = _scriptSegMap.getValOrDefault(script_nr, 0);
	if (*segid > 0) {
		return (Script *)_heap[*segid];
	}

	// allocate the SegmentObj
	SegmentObj *mem = allocSegment(new Script(), segid);

	// Add the script to the "script id -> segment id" hashmap
	_scriptSegMap[script_nr] = *segid;

	return (Script *)mem;
}

SegmentId SegManager::getActualSegment(SegmentId seg) const {
	if (getSciVersion() <= SCI_VERSION_2_1_LATE) {
		return seg;
	} else {
		// Return the lower 14 bits of the segment
		return (seg & 0x3FFF);
	}
}

void SegManager::deallocate(SegmentId seg) {
	SegmentId actualSegment = getActualSegment(seg);
	if (actualSegment < 1 || (uint)actualSegment >= _heap.size())
		error("Attempt to deallocate an invalid segment ID");

	SegmentObj *mobj = _heap[actualSegment];
	if (!mobj)
		error("Attempt to deallocate an already freed segment");

	if (mobj->getType() == SEG_TYPE_SCRIPT) {
		Script *scr = (Script *)mobj;
		_scriptSegMap.erase(scr->getScriptNumber());
		if (scr->getLocalsSegment()) {
			// Check if the locals segment has already been deallocated.
			// If the locals block has been stored in a segment with an ID
			// smaller than the segment ID of the script itself, it will be
			// already freed at this point. This can happen when scripts are
			// uninstantiated and instantiated again: they retain their own
			// segment ID, but are allocated a new locals segment, which can
			// have an ID smaller than the segment of the script itself.
			if (_heap[scr->getLocalsSegment()])
				deallocate(scr->getLocalsSegment());
		}
	}

	delete mobj;
	_heap[actualSegment] = NULL;
}

bool SegManager::isHeapObject(reg_t pos) const {
	const Object *obj = getObject(pos);
	if (obj == nullptr || obj->isFreed())
		return false;
	Script *scr = getScriptIfLoaded(pos.getSegment());
	return !(scr && scr->isMarkedAsDeleted());
}

void SegManager::deallocateScript(int script_nr) {
	deallocate(getScriptSegment(script_nr));
}

Script *SegManager::getScript(const SegmentId seg) {
	SegmentId actualSegment = getActualSegment(seg);
	if (actualSegment < 1 || (uint)actualSegment >= _heap.size()) {
		error("SegManager::getScript(): seg id %x out of bounds", actualSegment);
	}
	if (!_heap[actualSegment]) {
		error("SegManager::getScript(): seg id %x is not in memory", actualSegment);
	}
	if (_heap[actualSegment]->getType() != SEG_TYPE_SCRIPT) {
		error("SegManager::getScript(): seg id %x refers to type %d != SEG_TYPE_SCRIPT", actualSegment, _heap[actualSegment]->getType());
	}
	return (Script *)_heap[actualSegment];
}

Script *SegManager::getScriptIfLoaded(const SegmentId seg) const {
	SegmentId actualSegment = getActualSegment(seg);
	if (actualSegment < 1 || (uint)actualSegment >= _heap.size() || !_heap[actualSegment] || _heap[actualSegment]->getType() != SEG_TYPE_SCRIPT)
		return nullptr;
	return (Script *)_heap[actualSegment];
}

SegmentId SegManager::findSegmentByType(int type) const {
	for (uint i = 0; i < _heap.size(); i++)
		if (_heap[i] && _heap[i]->getType() == type)
			return i;
	return 0;
}

SegmentObj *SegManager::getSegmentObj(SegmentId seg) const {
	SegmentId actualSegment = getActualSegment(seg);
	if (actualSegment < 1 || (uint)actualSegment >= _heap.size() || !_heap[actualSegment])
		return nullptr;
	return _heap[actualSegment];
}

SegmentType SegManager::getSegmentType(SegmentId seg) const {
	SegmentId actualSegment = getActualSegment(seg);
	if (actualSegment < 1 || (uint)actualSegment >= _heap.size() || !_heap[actualSegment])
		return SEG_TYPE_INVALID;
	return _heap[actualSegment]->getType();
}

SegmentObj *SegManager::getSegment(SegmentId seg, SegmentType type) const {
	SegmentId actualSegment = getActualSegment(seg);
	return getSegmentType(actualSegment) == type ? _heap[actualSegment] : NULL;
}

Object *SegManager::getObject(reg_t pos) const {
	SegmentObj *mobj = getSegmentObj(pos.getSegment());
	Object *obj = nullptr;

	if (mobj != nullptr) {
		if (mobj->getType() == SEG_TYPE_CLONES) {
			CloneTable &ct = *(CloneTable *)mobj;
			if (ct.isValidEntry(pos.getOffset()))
				obj = &(ct[pos.getOffset()]);
			else
				warning("getObject(): Trying to get an invalid object");
		} else if (mobj->getType() == SEG_TYPE_SCRIPT) {
			Script *scr = (Script *)mobj;
			if (pos.getOffset() <= scr->getBufSize() && pos.getOffset() >= (uint)-SCRIPT_OBJECT_MAGIC_OFFSET
			        && scr->offsetIsObject(pos.getOffset())) {
				obj = scr->getObject(pos.getOffset());
			}
		}
	}

	return obj;
}

const char *SegManager::getObjectName(reg_t pos) {
	const Object *obj = getObject(pos);
	if (!obj)
		return "<no such object>";

	reg_t nameReg = obj->getNameSelector();
	if (nameReg.isNull())
		return "<no name>";

	const char *name = derefString(nameReg);

	if (!name) {
		return "<invalid name>";
	}

	return name;
}

Common::Array<reg_t> SegManager::findObjectsByName(const Common::String &name) {
	Common::Array<reg_t> result;

	// Now all values are available; iterate over all objects.
	for (uint i = 0; i < _heap.size(); i++) {
		const SegmentObj *mobj = _heap[i];

		if (!mobj)
			continue;

		reg_t objpos = make_reg(i, 0);

		if (mobj->getType() == SEG_TYPE_SCRIPT) {
			// It's a script, scan all objects in it
			const Script *scr = (const Script *)mobj;
			const ObjMap &objects = scr->getObjectMap();
			for (ObjMap::const_iterator it = objects.begin(); it != objects.end(); ++it) {
				objpos.setOffset(it->_value.getPos().getOffset());
				if (name == getObjectName(objpos))
					result.push_back(objpos);
			}
		} else if (mobj->getType() == SEG_TYPE_CLONES) {
			// It's clone table, scan all objects in it
			const CloneTable *ct = (const CloneTable *)mobj;
			for (uint idx = 0; idx < ct->size(); ++idx) {
				if (!ct->isValidEntry(idx))
					continue;

				objpos.setOffset(idx);
				if (name == getObjectName(objpos))
					result.push_back(objpos);
			}
		}
	}

	return result;
}

reg_t SegManager::findObjectByName(const Common::String &name, int index) {
	Common::Array<reg_t> result = findObjectsByName(name);

	if (result.empty())
		return NULL_REG;

	if (result.size() > 1 && index < 0) {
		debug("findObjectByName(%s): multiple matches:", name.c_str());
		for (uint i = 0; i < result.size(); i++)
			debug("  %3x: [%04x:%04x]", i, PRINT_REG(result[i]));
		return NULL_REG; // Ambiguous
	}

	if (index < 0)
		return result[0];
	else if (result.size() <= (uint)index)
		return NULL_REG; // Not found
	return result[index];
}

Common::Array<reg_t> SegManager::findObjectsBySuperClass(const Common::String &superClassName) {
	Common::Array<reg_t> result;

	reg_t superClass = findObjectByName(superClassName);
	if (superClass.isNull()) {
		return result;
	}

	// Now all values are available; iterate over all objects.
	for (uint i = 0; i < _heap.size(); i++) {
		const SegmentObj *mobj = _heap[i];

		if (!mobj)
			continue;

		reg_t objpos = make_reg(i, 0);

		if (mobj->getType() == SEG_TYPE_SCRIPT) {
			// It's a script, scan all objects in it
			const Script *scr = (const Script *)mobj;
			const ObjMap &objects = scr->getObjectMap();
			for (ObjMap::const_iterator it = objects.begin(); it != objects.end(); ++it) {
				objpos.setOffset(it->_value.getPos().getOffset());
				Object *object = getObject(objpos);
				if (object) {
					if (superClass == object->getSuperClassSelector()) {
						result.push_back(objpos);
					}
				}
			}
		} else if (mobj->getType() == SEG_TYPE_CLONES) {
			// It's clone table, scan all objects in it
			const CloneTable *ct = (const CloneTable *)mobj;
			for (uint idx = 0; idx < ct->size(); ++idx) {
				if (!ct->isValidEntry(idx))
					continue;

				objpos.setOffset(idx);
				Object *object = getObject(objpos);
				if (object) {
					if (superClass == object->getSuperClassSelector()) {
						result.push_back(objpos);
					}
				}
			}
		}
	}

	return result;
}

// return the seg if script_id is valid and in the map, else 0
SegmentId SegManager::getScriptSegment(int script_id) const {
	return _scriptSegMap.getValOrDefault(script_id, 0);
}

SegmentId SegManager::getScriptSegment(int script_nr, ScriptLoadType load, bool applyScriptPatches) {
	SegmentId segment;

	if ((load & SCRIPT_GET_LOAD) == SCRIPT_GET_LOAD)
		instantiateScript(script_nr, applyScriptPatches);

	segment = getScriptSegment(script_nr);

	if (segment > 0) {
		if ((load & SCRIPT_GET_LOCK) == SCRIPT_GET_LOCK)
			getScript(segment)->incrementLockers();
	}
	return segment;
}

DataStack *SegManager::allocateStack(int size, SegmentId *segid) {
	SegmentObj *mobj = allocSegment(new DataStack(), segid);
	DataStack *retval = (DataStack *)mobj;

	retval->_entries = (reg_t *)calloc(size, sizeof(reg_t));
	retval->_capacity = size;

	// SSCI initializes the stack with "S" characters (uppercase S in SCI0-SCI1,
	// lowercase s in SCI0 and SCI11) - probably stands for "stack"
	byte filler = (getSciVersion() >= SCI_VERSION_01 && getSciVersion() <= SCI_VERSION_1_LATE) ? 'S' : 's';
	for (int i = 0; i < size; i++)
		retval->_entries[i] = make_reg(0, filler);

	return retval;
}

void SegManager::freeHunkEntry(reg_t addr) {
	if (addr.isNull()) {
		warning("Attempt to free a Hunk from a null address");
		return;
	}

	HunkTable *ht = (HunkTable *)getSegment(addr.getSegment(), SEG_TYPE_HUNK);

	if (!ht) {
		warning("Attempt to free Hunk from address %04x:%04x: Invalid segment type %d", PRINT_REG(addr), getSegmentType(addr.getSegment()));
		return;
	}

	ht->freeEntryContents(addr.getOffset());
}

reg_t SegManager::allocateHunkEntry(const char *hunk_type, int size) {
	HunkTable *table;
	int offset;

	if (!_hunksSegId)
		allocSegment(new HunkTable(), &(_hunksSegId));
	table = (HunkTable *)_heap[_hunksSegId];

	offset = table->allocEntry();

	reg_t addr = make_reg(_hunksSegId, offset);
	Hunk &h = table->at(offset);

	h.mem = malloc(size);
	h.size = size;
	h.type = hunk_type;

	return addr;
}

byte *SegManager::getHunkPointer(reg_t addr) {
	HunkTable *ht = (HunkTable *)getSegment(addr.getSegment(), SEG_TYPE_HUNK);

	if (!ht || !ht->isValidEntry(addr.getOffset())) {
		// Valid SCI behavior, e.g. when loading/quitting
		return nullptr;
	}

	return (byte *)ht->at(addr.getOffset()).mem;
}

Clone *SegManager::allocateClone(reg_t *addr) {
	CloneTable *table;
	int offset;

	if (!_clonesSegId)
		table = (CloneTable *)allocSegment(new CloneTable(), &(_clonesSegId));
	else
		table = (CloneTable *)_heap[_clonesSegId];

	offset = table->allocEntry();

	*addr = make_reg(_clonesSegId, offset);
	return &table->at(offset);
}

List *SegManager::allocateList(reg_t *addr) {
	ListTable *table;
	int offset;

	if (!_listsSegId)
		allocSegment(new ListTable(), &(_listsSegId));
	table = (ListTable *)_heap[_listsSegId];

	offset = table->allocEntry();

	*addr = make_reg(_listsSegId, offset);
	return &table->at(offset);
}

Node *SegManager::allocateNode(reg_t *addr) {
	NodeTable *table;
	int offset;

	if (!_nodesSegId)
		allocSegment(new NodeTable(), &(_nodesSegId));
	table = (NodeTable *)_heap[_nodesSegId];

	offset = table->allocEntry();

	*addr = make_reg(_nodesSegId, offset);
	return &table->at(offset);
}

reg_t SegManager::newNode(reg_t value, reg_t key) {
	reg_t nodeRef;
	Node *n = allocateNode(&nodeRef);
	n->pred = n->succ = NULL_REG;
	n->key = key;
	n->value = value;

	return nodeRef;
}

List *SegManager::lookupList(reg_t addr) {
	if (getSegmentType(addr.getSegment()) != SEG_TYPE_LISTS) {
		error("Attempt to use non-list %04x:%04x as list", PRINT_REG(addr));
		return nullptr;
	}

	ListTable &lt = *(ListTable *)_heap[addr.getSegment()];

	if (!lt.isValidEntry(addr.getOffset())) {
		error("Attempt to use non-list %04x:%04x as list", PRINT_REG(addr));
		return nullptr;
	}

	return &(lt[addr.getOffset()]);
}

Node *SegManager::lookupNode(reg_t addr, bool stopOnDiscarded) {
	if (addr.isNull())
		return nullptr; // Non-error null

	SegmentType type = getSegmentType(addr.getSegment());

	if (type != SEG_TYPE_NODES) {
		error("Attempt to use non-node %04x:%04x (type %d) as list node", PRINT_REG(addr), type);
		return nullptr;
	}

	NodeTable &nt = *(NodeTable *)_heap[addr.getSegment()];

	if (!nt.isValidEntry(addr.getOffset())) {
		if (!stopOnDiscarded)
			return nullptr;

		error("Attempt to use invalid or discarded reference %04x:%04x as list node", PRINT_REG(addr));
		return nullptr;
	}

	return &(nt[addr.getOffset()]);
}

SegmentRef SegManager::dereference(reg_t pointer) {
	SegmentRef ret;

	if (!pointer.getSegment() || (pointer.getSegment() >= _heap.size()) || !_heap[pointer.getSegment()]) {
		// This occurs in KQ5CD when interacting with certain objects
		warning("SegManager::dereference(): Attempt to dereference invalid pointer %04x:%04x", PRINT_REG(pointer));
		return ret; /* Invalid */
	}

	SegmentObj *mobj = _heap[pointer.getSegment()];
	return mobj->dereference(pointer);
}

static void *derefPtr(SegManager *segMan, reg_t pointer, int entries, bool wantRaw) {
	SegmentRef ret = segMan->dereference(pointer);

	if (!ret.isValid())
		return nullptr;

	if (ret.isRaw != wantRaw) {
		warning("Dereferencing pointer %04x:%04x (type %d) which is %s, but expected %s", PRINT_REG(pointer),
			segMan->getSegmentType(pointer.getSegment()),
			ret.isRaw ? "raw" : "not raw",
			wantRaw ? "raw" : "not raw");
	}

	if (!wantRaw && ret.skipByte) {
		warning("Unaligned pointer read: %04x:%04x expected with word alignment", PRINT_REG(pointer));
		return nullptr;
	}

	if (entries > ret.maxSize) {
		warning("Trying to dereference pointer %04x:%04x beyond end of segment", PRINT_REG(pointer));
		return nullptr;
	}

	if (ret.isRaw)
		return ret.raw;
	else
		return ret.reg;
}

byte *SegManager::derefBulkPtr(reg_t pointer, int entries) {
	return (byte *)derefPtr(this, pointer, entries, true);
}

reg_t *SegManager::derefRegPtr(reg_t pointer, int entries) {
	return (reg_t *)derefPtr(this, pointer, 2*entries, false);
}

char *SegManager::derefString(reg_t pointer, int entries) {
	return (char *)derefPtr(this, pointer, entries, true);
}

// Helper functions for getting/setting characters in string fragments
static inline char getChar(const SegmentRef &ref, uint offset) {
	if (ref.skipByte)
		offset++;

	reg_t val = ref.reg[offset / 2];

	// segment 0xFFFF means that the scripts are using uninitialized temp-variable space
	//  we can safely ignore this, if it isn't one of the first 2 chars.
	//  foreign lsl3 uses kFileIO(readraw) and then immediately uses kReadNumber right at the start
	if (val.getSegment() != 0)
		if (!((val.getSegment() == 0xFFFF) && (offset > 1)))
			warning("Attempt to read character from non-raw data");

	bool oddOffset = offset & 1;
	if (g_sci->isBE())
		oddOffset = !oddOffset;

	return (oddOffset ? val.getOffset() >> 8 : val.getOffset() & 0xff);
}

static inline void setChar(const SegmentRef &ref, uint offset, byte value) {
	if (ref.skipByte)
		offset++;

	reg_t *val = ref.reg + offset / 2;

	val->setSegment(0);

	bool oddOffset = offset & 1;
	if (g_sci->isBE())
		oddOffset = !oddOffset;

	if (oddOffset)
		val->setOffset((val->getOffset() & 0x00ff) | (value << 8));
	else
		val->setOffset((val->getOffset() & 0xff00) | value);
}

template <bool STRING>
static void forwardCopy(byte *dest, const byte *src, size_t n) {
	const bool zeroPad = (STRING && n != 0xFFFFFFFFU);

	while (n) {
		--n;
		const byte b = *src++;
		*dest++ = b;
		if (STRING && b == '\0') {
			break;
		}
	}
	if (zeroPad) {
		while (n--) {
			*dest++ = '\0';
		}
	}
}

void SegManager::strncpy(reg_t dest, const char* src, size_t n) {
	SegmentRef dest_r = dereference(dest);
	if (!dest_r.isValid()) {
		warning("Attempt to strncpy to invalid pointer %04x:%04x", PRINT_REG(dest));
		return;
	}


	if (dest_r.isRaw) {
		forwardCopy<true>(dest_r.raw, (const byte *)src, n);
	} else {
		// raw -> non-raw
		for (uint i = 0; i < n; i++) {
			setChar(dest_r, i, src[i]);
			if (!src[i])
				break;
		}
		// Put an ending NUL to terminate the string
		if ((size_t)dest_r.maxSize > n)
			setChar(dest_r, n, 0);
	}
}

void SegManager::strncpy(reg_t dest, reg_t src, size_t n) {
	if (src.isNull()) {
		// Clear target string instead.
		if (n > 0)
			strcpy_(dest, "");

		return;	// empty text
	}

	SegmentRef dest_r = dereference(dest);
	const SegmentRef src_r = dereference(src);
	if (!src_r.isValid()) {
		warning("Attempt to strncpy from invalid pointer %04x:%04x", PRINT_REG(src));

		// Clear target string instead.
		if (n > 0)
			strcpy_(dest, "");
		return;
	}

	if (!dest_r.isValid()) {
		warning("Attempt to strncpy to invalid pointer %04x:%04x", PRINT_REG(dest));
		return;
	}


	if (src_r.isRaw) {
		// raw -> *
		strncpy(dest, (const char*)src_r.raw, n);
	} else if (dest_r.isRaw && !src_r.isRaw) {
		// non-raw -> raw
		for (uint i = 0; i < n; i++) {
			char c = getChar(src_r, i);
			dest_r.raw[i] = c;
			if (!c)
				break;
		}
	} else {
		// non-raw -> non-raw
		for (uint i = 0; i < n; i++) {
			char c = getChar(src_r, i);
			setChar(dest_r, i, c);
			if (!c)
				break;
		}
	}
}

void SegManager::strcpy_(reg_t dest, const char* src) {
	strncpy(dest, src, 0xFFFFFFFFU);
}

void SegManager::strcpy_(reg_t dest, reg_t src) {
	strncpy(dest, src, 0xFFFFFFFFU);
}

void SegManager::memcpy(reg_t dest, const byte* src, size_t n) {
	SegmentRef dest_r = dereference(dest);
	if (!dest_r.isValid()) {
		warning("Attempt to memcpy to invalid pointer %04x:%04x", PRINT_REG(dest));
		return;
	}
	if ((int)n > dest_r.maxSize) {
		warning("Trying to dereference pointer %04x:%04x beyond end of segment", PRINT_REG(dest));
		return;
	}

	if (dest_r.isRaw) {
		// raw -> raw
		forwardCopy<false>(dest_r.raw, src, n);
	} else {
		// raw -> non-raw
		for (uint i = 0; i < n; i++)
			setChar(dest_r, i, src[i]);
	}
}

void SegManager::memcpy(reg_t dest, reg_t src, size_t n) {
	SegmentRef dest_r = dereference(dest);
	const SegmentRef src_r = dereference(src);
	if (!dest_r.isValid()) {
		warning("Attempt to memcpy to invalid pointer %04x:%04x", PRINT_REG(dest));
		return;
	}
	if ((int)n > dest_r.maxSize) {
		warning("Trying to dereference pointer %04x:%04x beyond end of segment", PRINT_REG(dest));
		return;
	}
	if (!src_r.isValid()) {
		warning("Attempt to memcpy from invalid pointer %04x:%04x", PRINT_REG(src));
		return;
	}
	if ((int)n > src_r.maxSize) {
		warning("Trying to dereference pointer %04x:%04x beyond end of segment", PRINT_REG(src));
		return;
	}

	if (src_r.isRaw) {
		// raw -> *
		memcpy(dest, src_r.raw, n);
	} else if (dest_r.isRaw) {
		// * -> raw
		memcpy(dest_r.raw, src, n);
	} else {
		// non-raw -> non-raw
		for (uint i = 0; i < n; i++) {
			char c = getChar(src_r, i);
			setChar(dest_r, i, c);
		}
	}
}

void SegManager::memcpy(byte *dest, reg_t src, size_t n) {
	const SegmentRef src_r = dereference(src);
	if (!src_r.isValid()) {
		warning("Attempt to memcpy from invalid pointer %04x:%04x", PRINT_REG(src));
		return;
	}
	if ((int)n > src_r.maxSize) {
		warning("Trying to dereference pointer %04x:%04x beyond end of segment", PRINT_REG(src));
		return;
	}

	if (src_r.isRaw) {
		// raw -> raw
		forwardCopy<false>(dest, src_r.raw, n);
	} else {
		// non-raw -> raw
		for (uint i = 0; i < n; i++) {
			char c = getChar(src_r, i);
			dest[i] = c;
		}
	}
}

size_t SegManager::strlen(reg_t str) {
	if (str.isNull())
		return 0;	// empty text

	SegmentRef str_r = dereference(str);
	if (!str_r.isValid()) {
		warning("Attempt to call strlen on invalid pointer %04x:%04x", PRINT_REG(str));
		return 0;
	}

	if (str_r.isRaw) {
		// There is no guarantee that raw strings are zero-terminated; for
		// example, Phant1 reads "\r\n" from a pointer of size 2 during the
		// chase
		return Common::strnlen((const char *)str_r.raw, str_r.maxSize);
	} else {
		int i = 0;
		while (getChar(str_r, i))
			i++;
		return i;
	}
}


Common::String SegManager::getString(reg_t pointer) {
	Common::String ret;
	if (pointer.isNull())
		return ret;	// empty text

	SegmentRef src_r = dereference(pointer);
	if (!src_r.isValid()) {
		warning("SegManager::getString(): Attempt to dereference invalid pointer %04x:%04x", PRINT_REG(pointer));
		return ret;
	}

	if (src_r.isRaw) {
		// There is no guarantee that raw strings are zero-terminated; for
		// example, Phant1 reads "\r\n" from a pointer of size 2 during the
		// chase
		const uint size = Common::strnlen((const char *)src_r.raw, src_r.maxSize);
		ret = Common::String((const char *)src_r.raw, size);
	} else {
		uint i = 0;
		while (i < (uint)src_r.maxSize) {
			const char c = getChar(src_r, i);

			if (!c)
				break;

			i++;
			ret += c;
		}
	}
	return ret;
}

byte *SegManager::allocDynmem(int size, const char *descr, reg_t *addr) {
	SegmentId seg;
	SegmentObj *mobj = allocSegment(new DynMem(), &seg);
	*addr = make_reg(seg, 0);

	DynMem &d = *(DynMem *)mobj;

	d._size = size;

	// Original SCI only zeroed out heap memory on initialize
	// They didn't do it again for every allocation
	if (size) {
		d._buf = (byte *)calloc(size, 1);
	} else {
		d._buf = nullptr;
	}

	d._description = descr;

	return (byte *)(d._buf);
}

bool SegManager::freeDynmem(reg_t addr) {
	if (addr.getSegment() < 1 || addr.getSegment() >= _heap.size() ||
		!_heap[addr.getSegment()] || _heap[addr.getSegment()]->getType() != SEG_TYPE_DYNMEM)
		return false; // error

	deallocate(addr.getSegment());

	return true; // OK
}

#ifdef ENABLE_SCI32
#pragma mark -
#pragma mark Arrays

SciArray *SegManager::allocateArray(SciArrayType type, uint16 size, reg_t *addr) {
	ArrayTable *table;
	int offset;

	if (!_arraysSegId) {
		table = (ArrayTable *)allocSegment(new ArrayTable(), &(_arraysSegId));
	} else
		table = (ArrayTable *)_heap[_arraysSegId];

	offset = table->allocEntry();

	*addr = make_reg(_arraysSegId, offset);

	SciArray *array = &table->at(offset);
	array->setType(type);
	array->resize(size);
	return array;
}

SciArray *SegManager::lookupArray(reg_t addr) {
	if (_heap[addr.getSegment()]->getType() != SEG_TYPE_ARRAY)
		error("Attempt to use non-array %04x:%04x as array", PRINT_REG(addr));

	ArrayTable &arrayTable = *(ArrayTable *)_heap[addr.getSegment()];

	if (!arrayTable.isValidEntry(addr.getOffset()))
		error("Attempt to use non-array %04x:%04x as array", PRINT_REG(addr));

	return &(arrayTable[addr.getOffset()]);
}

void SegManager::freeArray(reg_t addr) {
	// SSCI memory manager ignores attempts to free null handles
	if (addr.isNull()) {
		return;
	}

	if (_heap[addr.getSegment()]->getType() != SEG_TYPE_ARRAY)
		error("Attempt to use non-array %04x:%04x as array", PRINT_REG(addr));

	ArrayTable &arrayTable = *(ArrayTable *)_heap[addr.getSegment()];

	if (!arrayTable.isValidEntry(addr.getOffset()))
		error("Attempt to use non-array %04x:%04x as array", PRINT_REG(addr));

	arrayTable.freeEntry(addr.getOffset());
}

bool SegManager::isArray(reg_t addr) const {
	return addr.getSegment() == _arraysSegId;
}

#pragma mark -
#pragma mark Bitmaps

SciBitmap *SegManager::allocateBitmap(reg_t *addr, const int16 width, const int16 height, const uint8 skipColor, const int16 originX, const int16 originY, const int16 xResolution, const int16 yResolution, const uint32 paletteSize, const bool remap, const bool gc) {
	BitmapTable *table;
	int offset;

	if (!_bitmapSegId) {
		table = (BitmapTable *)allocSegment(new BitmapTable(), &(_bitmapSegId));
	} else {
		table = (BitmapTable *)_heap[_bitmapSegId];
	}

	offset = table->allocEntry();

	*addr = make_reg(_bitmapSegId, offset);
	SciBitmap &bitmap = table->at(offset);

	bitmap.create(width, height, skipColor, originX, originY, xResolution, yResolution, paletteSize, remap, gc);

	return &bitmap;
}

SciBitmap *SegManager::lookupBitmap(const reg_t addr) {
	if (_heap[addr.getSegment()]->getType() != SEG_TYPE_BITMAP)
		error("Attempt to use non-bitmap %04x:%04x as bitmap", PRINT_REG(addr));

	BitmapTable &bitmapTable = *(BitmapTable *)_heap[addr.getSegment()];

	if (!bitmapTable.isValidEntry(addr.getOffset()))
		error("Attempt to use invalid entry %04x:%04x as bitmap", PRINT_REG(addr));

	return &(bitmapTable.at(addr.getOffset()));
}

void SegManager::freeBitmap(const reg_t addr) {
	if (_heap[addr.getSegment()]->getType() != SEG_TYPE_BITMAP)
		error("Attempt to free non-bitmap %04x:%04x as bitmap", PRINT_REG(addr));

	BitmapTable &bitmapTable = *(BitmapTable *)_heap[addr.getSegment()];

	if (!bitmapTable.isValidEntry(addr.getOffset()))
		error("Attempt to free invalid entry %04x:%04x as bitmap", PRINT_REG(addr));

	bitmapTable.freeEntry(addr.getOffset());
}

#pragma mark -

#endif

void SegManager::createClassTable() {
	Resource *vocab996 = _resMan->findResource(ResourceId(kResourceTypeVocab, 996), false);

	if (!vocab996)
		error("SegManager: failed to open vocab 996");

	int totalClasses = vocab996->size() >> 2;
	_classTable.resize(totalClasses);

	for (uint16 classNr = 0; classNr < totalClasses; classNr++) {
		uint16 scriptNr = vocab996->getUint16SEAt(classNr * 4 + 2);

		_classTable[classNr].reg = NULL_REG;
		_classTable[classNr].script = scriptNr;
	}
}

reg_t SegManager::getClassAddress(int classnr, ScriptLoadType lock, uint16 callerSegment, bool applyScriptPatches) {
	if (classnr == 0xffff)
		return NULL_REG;

	if (classnr < 0 || (int)_classTable.size() <= classnr || _classTable[classnr].script < 0) {
		error("[VM] Attempt to dereference class %x, which doesn't exist (max %x)", classnr, _classTable.size());
	} else {
		Class *the_class = &_classTable[classnr];
		if (!the_class->reg.getSegment()) {
			getScriptSegment(the_class->script, lock, applyScriptPatches);

			if (!the_class->reg.getSegment()) {
				if (lock == SCRIPT_GET_DONT_LOAD)
					return NULL_REG;

				error("[VM] Trying to instantiate class %x by instantiating script 0x%x (%03d) failed", classnr, the_class->script, the_class->script);
			}
		} else
			if (callerSegment != the_class->reg.getSegment())
				getScript(the_class->reg.getSegment())->incrementLockers();

		return the_class->reg;
	}
}

int SegManager::instantiateScript(int scriptNum, bool applyScriptPatches) {
	SegmentId segmentId = getScriptSegment(scriptNum);
	Script *scr = getScriptIfLoaded(segmentId);
	if (scr) {
		if (!scr->isMarkedAsDeleted()) {
			scr->incrementLockers();
			return segmentId;
		} else {
			scr->freeScript(true);
		}
	} else {
		scr = allocateScript(scriptNum, &segmentId);
	}

	scr->load(scriptNum, _resMan, _scriptPatcher, applyScriptPatches);
	scr->initializeLocals(this);
	scr->initializeClasses(this);
	scr->initializeObjects(this, segmentId, applyScriptPatches);
#ifdef ENABLE_SCI32
	g_sci->_guestAdditions->instantiateScriptHook(*scr);
#endif

	return segmentId;
}

void SegManager::uninstantiateScript(int script_nr) {
	SegmentId segmentId = getScriptSegment(script_nr);
	Script *scr = getScriptIfLoaded(segmentId);

	if (!scr || scr->isMarkedAsDeleted()) {   // Is it already unloaded?
		//warning("unloading script 0x%x requested although not loaded", script_nr);
		// This is perfectly valid SCI behavior
		return;
	}

	scr->decrementLockers();   // One less locker

	if (scr->getLockers() > 0)
		return;

	// Free all classtable references to this script
	for (uint i = 0; i < classTableSize(); i++)
		if (getClass(i).reg.getSegment() == segmentId)
			setClassOffset(i, NULL_REG);

	if (getSciVersion() < SCI_VERSION_1_1)
		uninstantiateScriptSci0(script_nr);
	// FIXME: Add proper script uninstantiation for SCI 1.1

	if (!scr->getLockers()) {
		// The actual script deletion seems to be done by SCI scripts themselves
		scr->markDeleted();
		debugC(kDebugLevelScripts, "Unloaded script 0x%x.", script_nr);
	}
}

void SegManager::uninstantiateScriptSci0(int script_nr) {
	bool oldScriptHeader = (getSciVersion() == SCI_VERSION_0_EARLY);
	SegmentId segmentId = getScriptSegment(script_nr);
	Script *scr = getScript(segmentId);
	reg_t reg = make_reg(segmentId, oldScriptHeader ? 2 : 0);
	int objLength = 0;

	// Make a pass over the object in order to uninstantiate all superclasses

	while (true) {
		reg.incOffset(objLength); // Step over the last checked object

		int objType = READ_SCI11ENDIAN_UINT16(scr->getBuf(reg.getOffset()));
		if (!objType)
			break;
		objLength = READ_SCI11ENDIAN_UINT16(scr->getBuf(reg.getOffset() + 2));

		reg.incOffset(4); // Step over header

		if ((objType == SCI_OBJ_OBJECT) || (objType == SCI_OBJ_CLASS)) { // object or class?
			reg.incOffset(8);	// magic offset (SCRIPT_OBJECT_MAGIC_OFFSET)
			int16 superclass = READ_SCI11ENDIAN_UINT16(scr->getBuf(reg.getOffset() + 2));

			if (superclass >= 0) {
				int superclass_script = getClass(superclass).script;

				if (superclass_script == script_nr) {
					if (scr->getLockers())
						scr->decrementLockers();  // Decrease lockers if this is us ourselves
				} else {
					uninstantiateScript(superclass_script);
				}
				// Recurse to assure that the superclass lockers number gets decreased
			}

			reg.incOffset(SCRIPT_OBJECT_MAGIC_OFFSET);
		} // if object or class

		reg.incOffset(-4); // Step back on header
	}
}

} // End of namespace Sci
