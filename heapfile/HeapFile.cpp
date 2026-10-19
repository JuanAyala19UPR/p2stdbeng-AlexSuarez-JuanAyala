#include "heapfile/HeapFile.h"


#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>



// ============================================================================
// HeapFile — STUDENT IMPLEMENTATION FILE
// ============================================================================
//
// Every member method declared in heapfile/HeapFile.h has a blank body
// below; the header documents the contract of each one (the file-level
// comment there explains the page format, the two free-space lists, and
// the pin protocol). Your task: replace the stub bodies with working
// implementations until the check suite (`p2stdbeng`) passes.
//
// What is already provided (do not change): the file-local helpers in the
// anonymous namespace below — the header-page layout constants, the
// HeaderFields struct and its read/write helpers, and the identification
// record writers/checkers. They fix the on-disk format; your member
// methods build on them. The free function erase_file at the bottom is
// also complete.

namespace heapfile {

namespace {

// Releases a pin without disturbing `error`, which on failure paths already
// carries the real cause (unpin clears the string it is given).
void release(bufman::BufferManager& manager, std::size_t frame, bool was_dirty) {
    std::string unpin_error;
    manager.unpin(frame, was_dirty, unpin_error);
}

// The header page's record area (its slots are never used): magic, the five
// structural fields, then the table name. Fixed-width fields come before
// the variable-length name so every field has a constant offset.
//
//   0   magic "TUP2"
//   4   free_head     first page with a free slot (kNoPage when empty)
//   8   free_tail     last page appended to the free list
//   12  full_head     first page whose every slot is used
//   16  full_tail
//   20  next_page_id  the id the next page allocation will use (starts at 1)
//   24  name_length
//   28  name bytes
struct HeaderFields {
    std::uint32_t free_head = kNoPage;
    std::uint32_t free_tail = kNoPage;
    std::uint32_t full_head = kNoPage;
    std::uint32_t full_tail = kNoPage;
    std::uint32_t next_page_id = 1;
};

constexpr std::uint32_t kHeapMagic = 0x32505554u; // "TUP2"
constexpr std::size_t kMagicOffset = 0;
constexpr std::size_t kHeaderFreeHeadOffset = 4;
constexpr std::size_t kHeaderFreeTailOffset = 8;
constexpr std::size_t kHeaderFullHeadOffset = 12;
constexpr std::size_t kHeaderFullTailOffset = 16;
constexpr std::size_t kHeaderNextPageIdOffset = 20;
constexpr std::size_t kNameLengthOffset = 24;
constexpr std::size_t kNameOffset = 28;
constexpr std::size_t kMaxTableNameLength = 200;

std::size_t head_offset(bool free_list) {
    return free_list ? kHeaderFreeHeadOffset : kHeaderFullHeadOffset;
}

std::size_t tail_offset(bool free_list) {
    return free_list ? kHeaderFreeTailOffset : kHeaderFullTailOffset;
}

HeaderFields read_header_fields(const char* buffer) {
    HeaderFields fields;
    fields.free_head = page_detail::read_u32(buffer, kHeaderFreeHeadOffset);
    fields.free_tail = page_detail::read_u32(buffer, kHeaderFreeTailOffset);
    fields.full_head = page_detail::read_u32(buffer, kHeaderFullHeadOffset);
    fields.full_tail = page_detail::read_u32(buffer, kHeaderFullTailOffset);
    fields.next_page_id = page_detail::read_u32(buffer, kHeaderNextPageIdOffset);
    return fields;
}

void write_header_fields(char* buffer, const HeaderFields& fields) {
    page_detail::write_u32(buffer, kHeaderFreeHeadOffset, fields.free_head);
    page_detail::write_u32(buffer, kHeaderFreeTailOffset, fields.free_tail);
    page_detail::write_u32(buffer, kHeaderFullHeadOffset, fields.full_head);
    page_detail::write_u32(buffer, kHeaderFullTailOffset, fields.full_tail);
    page_detail::write_u32(buffer, kHeaderNextPageIdOffset, fields.next_page_id);
}

// Writes the identification record into a bound header-page buffer: magic,
// empty lists, allocation frontier at 1, table name. Returns false with a
// named error when the table name is too long.
bool write_identification(char* buffer, const schema::TableSchema& schema,
                          std::string& error) {
    const std::string& name = schema.table_name();
    if (name.size() > kMaxTableNameLength) {
        error = "table name is " + std::to_string(name.size()) +
                " bytes; at most " + std::to_string(kMaxTableNameLength) +
                " fit the header page";
        return false;
    }
    page_detail::write_u32(buffer, kMagicOffset, kHeapMagic);
    write_header_fields(buffer, HeaderFields{});
    page_detail::write_u32(buffer, kNameLengthOffset,
                           static_cast<std::uint32_t>(name.size()));
    std::memcpy(buffer + kNameOffset, name.data(), name.size());
    return true;
}

// Checks the identification record against `schema`. False with a named
// error when the magic is missing or the name differs.
bool check_identification(const char* buffer,
                          const schema::TableSchema& schema,
                          std::string& error) {
    if (page_detail::read_u32(buffer, kMagicOffset) != kHeapMagic) {
        error = "not a tuple heap file";
        return false;
    }
    const std::uint32_t length = page_detail::read_u32(buffer, kNameLengthOffset);
    if (length > kMaxTableNameLength) {
        error = "not a tuple heap file";
        return false;
    }
    const std::string stored(buffer + kNameOffset, length);
    if (stored != schema.table_name()) {
        error = "heap file is for table '" + stored + "'";
        return false;
    }
    return true;
}

// The list pointers must name allocated pages (or be empty). A dirty page
// may not be on disk yet, so next_page_id can legitimately exceed the
// on-disk block count by at most the pool's frame count; anything beyond
// that is a corrupt header, not slack.
bool header_fields_in_range(const HeaderFields& fields, std::uint64_t blocks,
                            std::string& error) {
    constexpr std::uint64_t kDirtySlack = 4096; // >= any pool this engine builds
    const bool ok =
        fields.next_page_id >= 1 &&
        fields.next_page_id <= blocks + kDirtySlack &&
        (fields.free_head == kNoPage || fields.free_head < fields.next_page_id) &&
        (fields.free_tail == kNoPage || fields.free_tail < fields.next_page_id) &&
        (fields.full_head == kNoPage || fields.full_head < fields.next_page_id) &&
        (fields.full_tail == kNoPage || fields.full_tail < fields.next_page_id);
    if (!ok) {
        error = "header list pointers are out of range";
    }
    return ok;
}

}

// ---------------------------------------------------------------------------
// Member methods — YOUR WORK STARTS HERE
// ---------------------------------------------------------------------------

HeapFile::HeapFile(bufman::BufferManager& manager) : manager_(manager) {}

bool HeapFile::is_open() const {
    // TODO(student): report whether a file is currently open.
    return open_;
}

bool HeapFile::require_open(std::string& error) const {
    // TODO(student): fail with "heap file is not open" when closed.
    if (!open_) {
        error="heap file is not open";
        return false;
    }
    return true;
}

bool HeapFile::create(const std::string& path,
                      const schema::TableSchema& schema, std::string& error) {
    // TODO(student): format a one-block file: refuse an open HeapFile / a
    // non-empty path, validate the record size, alloc page 0, format it as
    // a SlottedPage, write the identification record, and close (flushing)
    // the file — leaving this HeapFile closed.
    schema::TupleSerializer serializer(schema);
    std::size_t record_size_=serializer.record_size();
    error.clear();
    if (open_) {
        error= "cannot create \""+ path + "\": this heap file is already open";
        return false;
    }

    bufman::File file;


    if (record_size_==0) {
        error= "schema has no record layout";
        return false;
    }
    if (slot_capacity(record_size_)==0) {
        error="record size does no fit a page";
        return false;
    }

    if (!manager_.open_file(path,file,error)) {
        return false;
    }
    const std::uint64_t blocks = bufman::block_count(file,error);
    if (!error.empty()) {
        std::string close_error;
        manager_.close_all(close_error);
        return false;
    }
    if (blocks>0) {
        error= "cannot create \"" + path +"\": the file already holds " +
        std::to_string(blocks) + "block(s)";
        std::string close_error;
        manager_.close_all(close_error);
        return false;
    }
    //Asignar la pagina 0, el header
    std::size_t frame = 0;
    if (!manager_.alloc_page(file,0,frame,error)) {
        std::string close_error;
        manager_.close_all(close_error);
        return false;
    }
    SlottedPage header;
    header.bind(manager_.frame(frame).buffer, record_size_);
    header.format();
    if (!write_identification(manager_.frame(frame).buffer,schema, error)) {
        release(manager_, frame, true);
        std::string close_error;
        manager_.close_all(close_error);
        return false;
    }
    release(manager_, frame, true);
    manager_.close_all(error);
    return error.empty();

}

bool HeapFile::open(const std::string& path,
                    const schema::TableSchema& schema, std::string& error) {
    // TODO(student): probe that the path exists, register it with the
    // manager, then validate block 0: identification, SlottedPage attach,
    // and the header fields in range. On any failure unwind completely
    // (close through the manager, reset the handle) and return false; on
    // success bind schema_/serializer_/record_size_ and mark the file open.
    (void)path;
    (void)schema;
    (void)error;
    return false;
}

HeapFileIterator HeapFile::scan() {
    // TODO(student): pin the header once, read the two list heads and
    // next_page_id, unpin, and return an iterator seeded with them. A
    // closed file yields an exhausted iterator.
    return HeapFileIterator(*this, kNoPage, kNoPage, 1);
}

// --- List surgery -----------------------------------------------------------

bool HeapFile::append_fresh_page(std::uint32_t page_id, std::size_t page_frame,
                                 std::string& error) {
    // TODO(student): make the just-allocated page the free list's only
    // element (head and tail) and advance next_page_id past it; clear the
    // page's own links. Only called right after an allocation, when the
    // free list is empty by construction — refuse otherwise.
    std::size_t frame_header=0;
    if (!manager_.pin(file_,0,frame_header,error)) {
        return false;

    }
    HeaderFields fields=read_header_fields(manager_.frame(frame_header).buffer);

    if (fields.free_head!=kNoPage) {
        release(manager_,frame_header,false);
        error="It can't append a new page because the free list is not empty";
        return false;
    }
    fields.free_head=page_id;
    fields.free_tail=page_id;
    fields.next_page_id=page_id+1;

    write_header_fields(manager_.frame(frame_header).buffer,fields);

    SlottedPage page;
    page.bind(manager_.frame(page_frame).buffer,record_size_);
    page.set_prev_page(kNoPage);
    page.set_next_page(kNoPage);

    release(manager_,frame_header,true);

    return true;
}

bool HeapFile::move_between_lists(bool to_full, std::uint32_t page_id,
                                  std::size_t page_frame, std::string& error) {
    // TODO(student): detach the pinned page from its source list (its
    // prev/next name the source neighbors) and append it at the
    // destination list's tail. Acquire ALL auxiliary pins first (header +
    // source neighbors + destination tail) in ascending page-id order;
    // after that the surgery is pure memory writes.

    ///PRIMERA PARTE QUITAR DE UNA
    ///LISTA LA PAGINA RECIBIDA COMO PARAMETRO
    auto header_to_full=head_offset(to_full);
    auto tail_to_full=tail_offset(to_full);

    //bindear la nueva pagina asignarle el frame pasado como parametro
    SlottedPage page_new;
    page_new.bind(manager_.frame(page_frame).buffer,record_size_);

    //Encontrar el id de la pagina next y de la previous
    auto page_new_next=page_new.next_page();
    auto page_new_prev=page_new.prev_page();

    //Pinear todas las paginas auxiliareas
    std::size_t frame_header=0;
    manager_.pin(file_,0,frame_header,error);

    std::size_t frame_previous_page=0;
    if (page_new_prev!=kNoPage) {
        manager_.pin(file_,page_new_prev,frame_previous_page,error);
    }
    std::size_t frame_next_page=0;
    if (page_new_next!=kNoPage) {
        manager_.pin(file_,page_new_next, frame_next_page,error);
    }
    auto page_head_new_list_offset=head_offset(!to_full);
    auto page_tail_new_list_offset=tail_offset(!to_full);
    auto page_tail_new_list=page_detail::read_u32(manager_.frame(frame_header).buffer, page_tail_new_list_offset);
    std::size_t frame_old_tail=0;
    if (page_tail_new_list!=kNoPage) {
        manager_.pin(file_, page_tail_new_list,frame_old_tail,error);
    }




    SlottedPage page_header;
    page_header.bind(manager_.frame(frame_header).buffer,record_size_);

    //Este caso es si el prev de la pagina a punta a kNoPage es decir no hay una pagina antes de el
    if (page_new_prev==kNoPage) {
        page_detail::write_u32(manager_.frame(frame_header).buffer,
            header_to_full,page_new_next);
    } else {
        //sino es asi tengo que bindear la pagina del previous para cambiarle su next
        SlottedPage previous_page;
        //std::size_t frame_previous_page;
        //manager_.pin(file_,page_new_prev,frame_previous_page,error);
        previous_page.bind(manager_.frame(frame_previous_page).buffer, record_size_);
        page_detail::write_u32(manager_.frame(frame_previous_page).buffer
            ,kNextPageIdOffset,page_new_next);

        //release hacer unpin y cambiarle el dirty para que haga flush
        //release(manager_, frame_previous_page,true);
    }
    ///Con el next
    if (page_new_next==kNoPage) {
        page_detail::write_u32(manager_.frame(frame_header).buffer, tail_to_full,page_new_prev);
    }else {
        //std::size_t frame_next_page=0;
        //manager_.pin(file_,page_new_next, frame_next_page,error);
        SlottedPage next_page;
        next_page.bind(manager_.frame(frame_next_page).buffer, record_size_);
        page_detail::write_u32(manager_.frame(frame_next_page).buffer,kPrevPageIdOffset,page_new_prev);
        //release(manager_,frame_next_page,true);
    }


    ///SEGUNDA PARTE AÑADIR A LA OTRA LISTA LA PAGINA RECIBIDA COMO PARAMETRO
    ///

    //auto page_head_new_list_offset=head_offset(!to_full);
    //auto page_tail_new_list_offset=tail_offset(!to_full);
    //auto page_tail_new_list=page_detail::read_u32(manager_.frame(frame_header).buffer, page_tail_new_list_offset);

    if (page_tail_new_list!=kNoPage) {
        //std::size_t frame_old_tail=0;
        //manager_.pin(file_, page_tail_new_list,frame_old_tail,error);
        SlottedPage page_tail_old;
        page_tail_old.bind(manager_.frame(frame_old_tail).buffer,record_size_);

        //Cambiar el next del old tail
        page_detail::write_u32(manager_.frame(frame_old_tail).buffer, kNextPageIdOffset, page_id);
        //release(manager_,frame_old_tail,true);
    }else {
        page_detail::write_u32(manager_.frame(frame_header).buffer,page_head_new_list_offset,page_id);
    }
    page_detail::write_u32(manager_.frame(frame_header).buffer,page_tail_new_list_offset,page_id);

    page_new.set_next_page(kNoPage);
    page_new.set_prev_page(page_tail_new_list);
    //Soltar los pines
    if (page_new_prev!=kNoPage) {
        release(manager_, frame_previous_page,true);
    }
    release(manager_, frame_header, true);
    if (page_tail_new_list!=kNoPage) {
        release(manager_,frame_old_tail,true);
    }
    if (page_new_next!=kNoPage) {
        release(manager_,frame_next_page,true);
    }
    return true;
}

// --- Row operations ---------------------------------------------------------

bool HeapFile::insert(const tuple::Tuple& tuple, RowId& row_id,
                      std::string& error) {
    // TODO(student): serialize (validates first), take the free list's
    // head as the target page — or allocate a fresh page and append it to
    // the free list when the free list is empty — store the record in the
    // target's first free slot, and move the page to the full list when
    // its last free slot was taken. Report the new row id.
    std::vector<char> record(record_size_);
    serializer_->serialize(tuple,record.data(), record.size(),error);


    (void)tuple;
    (void)row_id;
    (void)error;
    return false;
}

bool HeapFile::pin_record_page(const RowId row_id, SlottedPage& page,
                               std::size_t& frame, std::string& error) {
    // TODO(student): shared path for find/update/erase — reject the header
    // page and pages beyond the file's extent (resident pages count as
    // existing even before write-back), pin, attach, and require the slot
    // occupied. The caller owns the pin on success.
    (void)row_id;
    (void)page;
    (void)frame;
    (void)error;
    return false;
}

bool HeapFile::find(const RowId row_id, tuple::Tuple& tuple,
                    std::string& error) {
    // TODO(student): pin the record page, copy the record out, unpin, and
    // deserialize into `tuple`.
    (void)row_id;
    (void)tuple;
    (void)error;
    return false;
}

bool HeapFile::update(const RowId row_id, const tuple::Tuple& tuple,
                      std::string& error) {
    // TODO(student): serialize, pin the record page, overwrite the record
    // in place, and unpin. Occupancy never changes, so no list move.
    (void)row_id;
    (void)tuple;
    (void)error;
    return false;
}

bool HeapFile::erase(const RowId row_id, std::string& error) {
    // TODO(student): pin the record page, note whether the page WAS full,
    // clear the slot's occupancy byte, and move a just-unfilled page back
    // to the free list (at the tail).
    (void)row_id;
    (void)error;
    return false;
}

// --- Structure self-check ---------------------------------------------------

bool HeapFile::check_structure(std::string& error) {
    // TODO(student): walk both lists one pin at a time and verify every
    // invariant the header documents: heads/tails agree with the walk,
    // every allocated page reachable exactly once, membership matches
    // occupancy, page 0 on no list, the union covering exactly pages
    // 1..next_page_id-1, and every on-disk data page reachable.
    (void)error;
    return false;
}

// --- Lifecycle --------------------------------------------------------------

bool HeapFile::resident(std::uint32_t page_id) const {
    // TODO(student): observe the manager's pool.
    (void)page_id;
    return false;
}

bool HeapFile::close(std::string& error) {
    // TODO(student): close the file through the manager; reset this
    // HeapFile's state only when that succeeded, so a failed close can be
    // retried.
    if (!require_open(error)) {
        return false;
    }
    if (!manager_.close_file(file_,error)) {
        return false;
    }
    open_ = false;
    schema_=nullptr;
    serializer_.reset();
    record_size_=0;
    return true;
    }



// --- HeapFileIterator -------------------------------------------------------

HeapFileIterator::HeapFileIterator(HeapFile& file, std::uint32_t free_head,
                                   std::uint32_t full_head,
                                   std::uint32_t allocated)
    : file_(&file),
      page_id_(free_head),
      next_list_(full_head),
      slot_(0),
      seen_(allocated, 0) {}

bool HeapFileIterator::next(tuple::Tuple& tuple, std::string& error) {
    // TODO(student): walk the free list's pages, then the full list's, in
    // list order; on each page scan slots in order and yield every
    // occupied slot's decoded record, remembering the slot to resume from.
    // Exhaustion is false with an EMPTY error; a revisited page means a
    // cyclic list and is a named failure.
    (void)tuple;
    (void)error;
    return false;
}

// --- Free functions ---------------------------------------------------------

bool erase_file(bufman::BufferManager& manager, const std::string& path,
                std::string& error) {
    error.clear();
    if (manager.is_open(path)) {
        error = "cannot erase \"" + path + "\": the file is open";
        return false;
    }
    if (std::remove(path.c_str()) != 0) {
        error = "cannot erase \"" + path + "\": " + std::strerror(errno);
        return false;
    }
    return true;
}

}
