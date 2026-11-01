#include "Archive.h"

#include <7z.h>
#include <7zAlloc.h>
#include <7zCrc.h>
#include <Lzma2Dec.h>
#include <LzmaDec.h>

#include <mutex>

namespace lithium::packs
{

namespace
{
    const juce::String cancelled { "Cancelled" };
    constexpr int chunkSize = 1 << 20;

    // The LZMA SDK's allocator: plain malloc and free.
    const ISzAlloc sdkAlloc { SzAlloc, SzFree };

    /** CRC-32 (the zip one), from the LZMA SDK, whose table is built once.
        This is the running value: start from CRC_INIT_VAL, and finish with
        CRC_GET_DIGEST to get the checksum. */
    std::uint32_t crc32 (std::uint32_t crc, const void* data, size_t size)
    {
        static std::once_flag tableBuilt;
        std::call_once (tableBuilt, [] { CrcGenerateTable(); });
        return CrcUpdate (crc, data, size);
    }

    /** Where an entry goes, or no file if its name would take it outside the
        destination: an absolute path, a drive letter, or "..". */
    juce::File safeDestination (const juce::File& root, const juce::String& entryName)
    {
        const auto name = entryName.replaceCharacter ('\\', '/');

        if (name.startsWithChar ('/'))
            return {};

        juce::StringArray parts;
        parts.addTokens (name, "/", {});
        parts.removeEmptyStrings();
        parts.removeString (".");

        if (parts.isEmpty() || parts.contains ("..") || parts[0].endsWithChar (':'))
            return {};

        return root.getChildFile (parts.joinIntoString ("/"));
    }

    /** Copies `length` bytes (or until the stream ends, if negative) into a
        file. reportProgress gets the bytes written so far. */
    juce::Result writeFile (juce::InputStream& in, std::int64_t length, const juce::File& dest,
                            const std::function<bool (std::int64_t)>& reportProgress, std::uint32_t* crc = nullptr)
    {
        if (! dest.getParentDirectory().createDirectory())
            return juce::Result::fail ("Can't create " + dest.getParentDirectory().getFullPathName());

        dest.deleteFile();
        juce::FileOutputStream out (dest, chunkSize);

        if (out.failedToOpen())
            return juce::Result::fail ("Can't write " + dest.getFullPathName());

        juce::HeapBlock<char> buffer (chunkSize);
        std::int64_t written = 0;

        while (length < 0 || written < length)
        {
            const auto wanted = (int) (length < 0 ? chunkSize : std::min<std::int64_t> (chunkSize, length - written));
            const auto got = in.read (buffer, wanted);

            if (got <= 0)
            {
                if (length < 0)
                    break;

                return juce::Result::fail ("The archive ends early, in " + dest.getFileName());
            }

            if (crc != nullptr)
                *crc = crc32 (*crc, buffer, (size_t) got);

            if (! out.write (buffer, (size_t) got))
                return juce::Result::fail ("Can't write " + dest.getFullPathName() + " (is the disk full?)");

            written += got;

            if (reportProgress && ! reportProgress (written))
                return juce::Result::fail (cancelled);
        }

        out.flush();
        return out.getStatus();
    }

    std::uint16_t u16 (const std::uint8_t* p)    { return (std::uint16_t) (p[0] | p[1] << 8); }
    std::uint32_t u32 (const std::uint8_t* p)    { return (std::uint32_t) u16 (p) | (std::uint32_t) u16 (p + 2) << 16; }
    std::uint64_t u64 (const std::uint8_t* p)    { return (std::uint64_t) u32 (p) | (std::uint64_t) u32 (p + 4) << 32; }

    //==============================================================================
    // Tar

    std::int64_t tarNumber (const std::uint8_t* field, int length)
    {
        // Big files store their size in base-256, flagged by the top bit.
        if ((field[0] & 0x80) != 0)
        {
            std::int64_t v = field[0] & 0x7f;

            for (int i = 1; i < length; ++i)
                v = (v << 8) | field[i];

            return v;
        }

        std::int64_t v = 0;

        for (int i = 0; i < length && field[i] != 0; ++i)
            if (field[i] >= '0' && field[i] <= '7')
                v = v * 8 + (field[i] - '0');

        return v;
    }

    juce::String tarString (const std::uint8_t* field, int length)
    {
        int n = 0;

        while (n < length && field[n] != 0)
            ++n;

        return juce::String::fromUTF8 (reinterpret_cast<const char*> (field), n);
    }

    /** Unpacks a tar stream. fraction() says how far through the archive it is. */
    juce::Result extractTar (juce::InputStream& in, const juce::File& root, const std::function<double()>& fraction,
                             const Progress& progress)
    {
        std::uint8_t header[512];
        juce::String longName;

        auto readBlock = [&in] (void* dest, int size) { return in.read (dest, size) == size; };
        auto skip = [&in] (std::int64_t bytes)
        {
            juce::HeapBlock<char> buffer (chunkSize);

            while (bytes > 0)
            {
                const auto got = in.read (buffer, (int) std::min<std::int64_t> (bytes, chunkSize));

                if (got <= 0)
                    return false;

                bytes -= got;
            }

            return true;
        };
        auto padding = [] (std::int64_t size) { return (512 - size % 512) % 512; };

        for (;;)
        {
            if (! readBlock (header, 512))
                return juce::Result::fail ("The tar archive ends early");

            if (std::all_of (std::begin (header), std::end (header), [] (std::uint8_t b) { return b == 0; }))
                return juce::Result::ok();   // the end marker

            const auto size = tarNumber (header + 124, 12);
            const auto type = (char) header[156];
            auto name = tarString (header, 100);

            if (tarString (header + 257, 5) == "ustar")
                if (const auto prefix = tarString (header + 345, 155); prefix.isNotEmpty())
                    name = prefix + "/" + name;

            if (longName.isNotEmpty())
                name = std::exchange (longName, {});

            if (type == 'L' || type == 'x')
            {
                // A long name: GNU's comes as the next entry's data, POSIX's as a "path" record.
                if (size > (1 << 20))
                    return juce::Result::fail ("A broken long name in the tar archive");

                juce::MemoryBlock data ((size_t) size);

                if (! readBlock (data.getData(), (int) size) || ! skip (padding (size)))
                    return juce::Result::fail ("The tar archive ends early");

                const auto text = juce::String::fromUTF8 (static_cast<const char*> (data.getData()), (int) size);

                if (type == 'L')
                {
                    longName = text;   // the name's terminating NUL already ended the string
                }
                else
                {
                    for (const auto& record : juce::StringArray::fromLines (text))
                        if (const auto value = record.fromFirstOccurrenceOf (" path=", false, false); value.isNotEmpty())
                            longName = value;
                }

                continue;
            }

            const auto dest = safeDestination (root, name);

            if (type == '5')
            {
                if (dest != juce::File())
                    dest.createDirectory();
            }
            else if ((type == '0' || type == 0 || type == '7') && dest != juce::File())
            {
                const auto r = writeFile (in, size, dest, [&] (std::int64_t) { return ! progress || progress (fraction()); });

                if (r.failed())
                    return r;

                if (! skip (padding (size)))
                    return juce::Result::fail ("The tar archive ends early");

                continue;
            }

            // Links, devices, and anything that would land outside the destination: skipped.
            if (! skip (size + padding (size)))
                return juce::Result::fail ("The tar archive ends early");
        }
    }

    //==============================================================================
    // Zip

    struct ZipEntry
    {
        juce::String name;
        int method = 0;
        std::uint32_t crc = 0;
        std::int64_t compressedSize = 0, size = 0, localOffset = 0;
        bool encrypted = false, isLink = false;
    };

    juce::Result readZipDirectory (juce::FileInputStream& in, std::vector<ZipEntry>& entries)
    {
        const auto length = in.getTotalLength();
        const auto tailSize = (int) std::min<std::int64_t> (length, 22 + 65535);
        juce::MemoryBlock tail ((size_t) tailSize);

        if (! in.setPosition (length - tailSize) || in.read (tail.getData(), tailSize) != tailSize)
            return juce::Result::fail ("Can't read the zip file");

        // The end-of-directory record, searched for from the end (a comment can follow it).
        const auto* t = static_cast<const std::uint8_t*> (tail.getData());
        int eocd = -1;

        for (int i = tailSize - 22; i >= 0 && eocd < 0; --i)
            if (u32 (t + i) == 0x06054b50)
                eocd = i;

        if (eocd < 0)
            return juce::Result::fail ("Not a zip file, or a damaged one");

        std::uint64_t count = u16 (t + eocd + 10);
        std::uint64_t directorySize = u32 (t + eocd + 12);
        std::uint64_t directoryOffset = u32 (t + eocd + 16);

        // Zip64: the real numbers are in another record, found through a locator just before this one.
        if (count == 0xffff || directorySize == 0xffffffff || directoryOffset == 0xffffffff)
        {
            const auto eocdPosition = length - tailSize + eocd;
            std::uint8_t locator[20], record[56];

            if (eocdPosition < 20 || ! in.setPosition (eocdPosition - 20) || in.read (locator, 20) != 20 || u32 (locator) != 0x07064b50
                || ! in.setPosition ((std::int64_t) u64 (locator + 8)) || in.read (record, 56) != 56 || u32 (record) != 0x06064b50)
                return juce::Result::fail ("A damaged Zip64 directory");

            count = u64 (record + 32);
            directorySize = u64 (record + 40);
            directoryOffset = u64 (record + 48);
        }

        if (directorySize > (512u << 20) || directoryOffset + directorySize > (std::uint64_t) length)
            return juce::Result::fail ("A damaged zip directory");

        juce::MemoryBlock directory ((size_t) directorySize);

        if (! in.setPosition ((std::int64_t) directoryOffset) || in.read (directory.getData(), (int) directorySize) != (int) directorySize)
            return juce::Result::fail ("Can't read the zip directory");

        const auto* d = static_cast<const std::uint8_t*> (directory.getData());
        const auto* end = d + directorySize;

        for (std::uint64_t i = 0; i < count; ++i)
        {
            if (d + 46 > end || u32 (d) != 0x02014b50)
                return juce::Result::fail ("A damaged zip directory");

            ZipEntry e;
            const auto madeBy = u16 (d + 4);
            const auto flags = u16 (d + 8);
            e.method = u16 (d + 10);
            e.crc = u32 (d + 16);
            e.compressedSize = u32 (d + 20);
            e.size = u32 (d + 24);
            const auto nameLength = u16 (d + 28), extraLength = u16 (d + 30), commentLength = u16 (d + 32);
            const auto attributes = u32 (d + 38);
            e.localOffset = u32 (d + 42);
            e.encrypted = (flags & 1) != 0;

            // Unix symlinks (made on a Unix host, file type 0120000) aren't followed or made.
            e.isLink = (madeBy >> 8) == 3 && ((attributes >> 16) & 0170000) == 0120000;

            if (d + 46 + nameLength + extraLength + commentLength > end)
                return juce::Result::fail ("A damaged zip directory");

            // Names are UTF-8 when flagged, else the old DOS code page; plain ASCII either way for these libraries.
            const auto* namePtr = reinterpret_cast<const char*> (d + 46);
            e.name = (flags & 0x800) != 0 ? juce::String::fromUTF8 (namePtr, nameLength)
                                          : juce::String (juce::CharPointer_ASCII (namePtr), (size_t) nameLength);

            // Zip64 sizes and offset, for those that didn't fit in 32 bits, in this order.
            for (const auto* x = d + 46 + nameLength; x + 4 <= d + 46 + nameLength + extraLength;)
            {
                const auto id = u16 (x), size = u16 (x + 2);
                const auto* v = x + 4;

                if (id == 0x0001)
                {
                    if (e.size == 0xffffffff && v + 8 <= x + 4 + size)              { e.size = (std::int64_t) u64 (v); v += 8; }
                    if (e.compressedSize == 0xffffffff && v + 8 <= x + 4 + size)    { e.compressedSize = (std::int64_t) u64 (v); v += 8; }
                    if (e.localOffset == 0xffffffff && v + 8 <= x + 4 + size)       { e.localOffset = (std::int64_t) u64 (v); }
                }

                x += 4 + size;
            }

            entries.push_back (std::move (e));
            d += 46 + nameLength + extraLength + commentLength;
        }

        return juce::Result::ok();
    }

    juce::Result extractZip (const juce::File& archive, const juce::File& root, const Progress& progress)
    {
        juce::FileInputStream in (archive);

        if (in.failedToOpen())
            return juce::Result::fail ("Can't open " + archive.getFullPathName());

        std::vector<ZipEntry> entries;

        if (const auto r = readZipDirectory (in, entries); r.failed())
            return juce::Result::fail (archive.getFileName() + ": " + r.getErrorMessage());

        std::int64_t total = 0, done = 0;

        for (const auto& e : entries)
            total += e.compressedSize;

        for (const auto& e : entries)
        {
            const auto dest = safeDestination (root, e.name);

            // macOS adds resource forks under __MACOSX; they're of no use here.
            if (dest == juce::File() || e.isLink || e.name.startsWith ("__MACOSX/"))
            {
                done += e.compressedSize;
                continue;
            }

            if (e.name.endsWithChar ('/'))
            {
                dest.createDirectory();
                continue;
            }

            if (e.encrypted)
                return juce::Result::fail (e.name + " is encrypted");

            if (e.method != 0 && e.method != 8)
                return juce::Result::fail (e.name + " uses a compression method other than deflate");

            // The data starts after the entry's local header, whose name and extra field can differ from the directory's.
            std::uint8_t local[30];

            if (! in.setPosition (e.localOffset) || in.read (local, 30) != 30 || u32 (local) != 0x04034b50)
                return juce::Result::fail ("A damaged entry: " + e.name);

            const auto dataStart = e.localOffset + 30 + u16 (local + 26) + u16 (local + 28);
            juce::SubregionStream region (&in, dataStart, e.compressedSize, false);
            std::unique_ptr<juce::InputStream> inflater;

            if (e.method == 8)
                inflater = std::make_unique<juce::GZIPDecompressorInputStream> (&region, false, juce::GZIPDecompressorInputStream::deflateFormat, e.size);

            auto& source = inflater != nullptr ? *inflater : static_cast<juce::InputStream&> (region);
            std::uint32_t crc = CRC_INIT_VAL;
            const auto r = writeFile (source, e.size, dest, [&] (std::int64_t written)
            {
                const auto fraction = e.size > 0 ? (double) written / (double) e.size : 1.0;
                return ! progress || progress ((done + fraction * (double) e.compressedSize) / (double) std::max<std::int64_t> (1, total));
            }, &crc);

            if (r.failed())
                return r;

            if (CRC_GET_DIGEST (crc) != e.crc)
                return juce::Result::fail (e.name + " is damaged (its checksum doesn't match): download it again");

            done += e.compressedSize;
        }

        return juce::Result::ok();
    }

    //==============================================================================
    // 7z

    /** juce::FileInputStream as the LZMA SDK's seekable input. The SDK's
        interface is a C struct of function pointers, first in this one. */
    struct SeekStream
    {
        ISeekInStream vt;
        juce::FileInputStream* file;

        explicit SeekStream (juce::FileInputStream& f) : file (&f)
        {
            vt.Read = [] (ISeekInStreamPtr p, void* buf, size_t* size) -> SRes
            {
                auto* self = reinterpret_cast<const SeekStream*> (p);
                const auto got = self->file->read (buf, (int) std::min<size_t> (*size, 1 << 30));
                *size = (size_t) std::max (0, got);
                return got < 0 ? SZ_ERROR_READ : SZ_OK;
            };

            vt.Seek = [] (ISeekInStreamPtr p, Int64* pos, ESzSeek origin) -> SRes
            {
                auto* self = reinterpret_cast<const SeekStream*> (p);
                const auto base = origin == SZ_SEEK_SET ? 0 : (origin == SZ_SEEK_CUR ? self->file->getPosition() : self->file->getTotalLength());

                if (! self->file->setPosition (base + *pos))
                    return SZ_ERROR_READ;

                *pos = self->file->getPosition();
                return SZ_OK;
            };
        }
    };

    /** One LZMA or LZMA2 stream of a 7z archive, decoded as it's read. */
    class LzmaStream final : public juce::InputStream
    {
    public:
        LzmaStream (juce::FileInputStream& f, std::int64_t packStart, std::int64_t packSize, bool lzma2,
                    const Byte* props, unsigned propsSize, std::int64_t unpackSize)
            : file (f), packPosition (packStart), packEnd (packStart + packSize), isLzma2 (lzma2), length (unpackSize)
        {
            if (isLzma2)
            {
                Lzma2Dec_CONSTRUCT (&lzma2Decoder)
                ok = propsSize == 1 && Lzma2Dec_Allocate (&lzma2Decoder, props[0], &sdkAlloc) == SZ_OK;

                if (ok)
                    Lzma2Dec_Init (&lzma2Decoder);
            }
            else
            {
                LzmaDec_CONSTRUCT (&lzmaDecoder)
                ok = LzmaDec_Allocate (&lzmaDecoder, props, propsSize, &sdkAlloc) == SZ_OK;

                if (ok)
                    LzmaDec_Init (&lzmaDecoder);
            }
        }

        ~LzmaStream() override
        {
            if (isLzma2)
                Lzma2Dec_Free (&lzma2Decoder, &sdkAlloc);
            else
                LzmaDec_Free (&lzmaDecoder, &sdkAlloc);
        }

        bool isValid() const noexcept                  { return ok; }
        double packedFraction (std::int64_t packStart) const
        {
            return (double) (packPosition - packStart) / (double) std::max<std::int64_t> (1, packEnd - packStart);
        }

        juce::int64 getTotalLength() override          { return length; }
        bool isExhausted() override                    { return produced >= length; }
        juce::int64 getPosition() override             { return produced; }
        // Only forwards, as it decodes; "staying put" is all juce::SubregionStream asks of it.
        bool setPosition (juce::int64 pos) override    { return pos == produced; }

        int read (void* dest, int bytes) override
        {
            auto* out = static_cast<Byte*> (dest);
            int total = 0;

            while (ok && total < bytes && produced < length)
            {
                if (inPos == inSize && packPosition < packEnd)
                {
                    const auto wanted = (int) std::min<std::int64_t> (chunkSize, packEnd - packPosition);

                    if (! file.setPosition (packPosition) || file.read (input, wanted) != wanted)
                        return failed();

                    packPosition += wanted;
                    inPos = 0;
                    inSize = (size_t) wanted;
                }

                auto outLen = (SizeT) std::min<std::int64_t> (bytes - total, length - produced);
                auto inLen = (SizeT) (inSize - inPos);
                ELzmaStatus status;
                const auto res = isLzma2 ? Lzma2Dec_DecodeToBuf (&lzma2Decoder, out + total, &outLen, input + inPos, &inLen, LZMA_FINISH_ANY, &status)
                                         : LzmaDec_DecodeToBuf (&lzmaDecoder, out + total, &outLen, input + inPos, &inLen, LZMA_FINISH_ANY, &status);

                if (res != SZ_OK || (outLen == 0 && inLen == 0))
                    return failed();

                inPos += inLen;
                total += (int) outLen;
                produced += (std::int64_t) outLen;
            }

            return total;
        }

    private:
        int failed()
        {
            ok = false;
            return -1;
        }

        juce::FileInputStream& file;
        std::int64_t packPosition, packEnd;
        const bool isLzma2;
        const std::int64_t length;
        std::int64_t produced = 0;
        CLzmaDec lzmaDecoder {};
        CLzma2Dec lzma2Decoder {};
        juce::HeapBlock<Byte> input { (size_t) chunkSize };
        size_t inPos = 0, inSize = 0;
        bool ok = false;
    };

    juce::String fileName (const CSzArEx& db, size_t index)
    {
        std::vector<UInt16> utf16 (SzArEx_GetFileNameUtf16 (&db, index, nullptr));
        SzArEx_GetFileNameUtf16 (&db, index, utf16.data());
        return juce::String (juce::CharPointer_UTF16 (reinterpret_cast<const juce::CharPointer_UTF16::CharType*> (utf16.data())));
    }

    juce::Result extract7z (const juce::File& archive, const juce::File& root, const Progress& progress)
    {
        juce::FileInputStream in (archive);

        if (in.failedToOpen())
            return juce::Result::fail ("Can't open " + archive.getFullPathName());

        crc32 (0, nullptr, 0);   // the SDK checks CRCs with the table this builds

        SeekStream seek (in);
        CLookToRead2 look {};
        LookToRead2_CreateVTable (&look, False);
        juce::HeapBlock<Byte> lookBuffer (1 << 18);
        look.buf = lookBuffer;
        look.bufSize = 1 << 18;
        look.realStream = &seek.vt;
        LookToRead2_INIT (&look)

        CSzArEx db;
        SzArEx_Init (&db);

        // The SDK's database is C; this frees it however the function returns.
        const std::unique_ptr<CSzArEx, void (*) (CSzArEx*)> freeDb (&db, [] (CSzArEx* p) { SzArEx_Free (p, &sdkAlloc); });

        if (SzArEx_Open (&db, &look.vt, &sdkAlloc, &sdkAlloc) != SZ_OK)
            return juce::Result::fail (archive.getFileName() + " isn't a 7z archive, or is damaged");

        const auto archiveLength = (double) std::max<std::int64_t> (1, in.getTotalLength());

        for (UInt32 f = 0; f < db.db.NumFolders; ++f)
        {
            CSzFolder folder;
            const auto* coders = db.db.CodersData + db.db.FoCodersOffsets[f];
            CSzData sd { coders, db.db.FoCodersOffsets[f + 1] - db.db.FoCodersOffsets[f] };

            if (SzGetNextFolderItem (&folder, &sd) != SZ_OK)
                return juce::Result::fail ("A damaged 7z archive");

            const auto packIndex = db.db.FoStartPackStreamIndex[f];
            const auto packStart = (std::int64_t) (db.dataPos + db.db.PackPositions[packIndex]);
            const auto packSize = (std::int64_t) (db.db.PackPositions[packIndex + 1] - db.db.PackPositions[packIndex]);
            const auto& coder = folder.Coders[0];
            constexpr UInt32 lzmaId = 0x030101, lzma2Id = 0x21;

            // Plain LZMA or LZMA2 (what 7-Zip uses unless told otherwise) is streamed. Filter chains (BCJ for
            // programs) aren't: no drum library needs them.
            if (folder.NumCoders != 1 || folder.NumPackStreams != 1 || (coder.MethodID != lzmaId && coder.MethodID != lzma2Id))
                return juce::Result::fail (archive.getFileName() + " uses 7z compression settings Lithium Drums can't unpack");

            LzmaStream stream (in, packStart, packSize, coder.MethodID == lzma2Id, coders + coder.PropsOffset, coder.PropsSize,
                               (std::int64_t) SzAr_GetFolderUnpackSize (&db.db, f));

            if (! stream.isValid())
                return juce::Result::fail ("Can't start decoding " + archive.getFileName());

            // A folder's files are one after another in its stream.
            for (auto i = db.FolderToFile[f]; i < db.FolderToFile[f + 1]; ++i)
            {
                // An archive made from a stream ("7z a -si") stores no name; 7-Zip then names the file after
                // the archive, as here: salamander_drumkit_v1.tar.7z holds salamander_drumkit_v1.tar.
                auto name = fileName (db, i);

                if (name.isEmpty())
                    name = archive.getFileNameWithoutExtension();

                const auto size = (std::int64_t) SzArEx_GetFileSize (&db, i);
                const auto fraction = [&] { return ((double) packStart + stream.packedFraction (packStart) * (double) packSize) / archiveLength; };

                if (SzArEx_IsDir (&db, i))
                    continue;

                if (name.endsWithIgnoreCase (".tar"))
                {
                    // A .tar.7z: the tar is unpacked as it's decoded, never written out.
                    juce::SubregionStream tar (&stream, stream.getPosition(), size, false);

                    if (const auto r = extractTar (tar, root, fraction, progress); r.failed())
                        return r;

                    // Tar archives end with padding the tar reader doesn't read; skip it.
                    juce::HeapBlock<char> rest (chunkSize);

                    while (tar.getPosition() < size && tar.read (rest, chunkSize) > 0) {}

                    continue;
                }

                const auto dest = safeDestination (root, name);

                if (dest == juce::File())
                    return juce::Result::fail ("An entry would land outside the folder: " + name);

                juce::SubregionStream file (&stream, stream.getPosition(), size, false);

                if (const auto r = writeFile (file, size, dest, [&] (std::int64_t) { return ! progress || progress (fraction()); }); r.failed())
                    return r;
            }

            for (auto i = db.FolderToFile[f]; i < db.FolderToFile[f + 1]; ++i)
                if (SzArEx_IsDir (&db, i))
                    if (const auto dest = safeDestination (root, fileName (db, i)); dest != juce::File())
                        dest.createDirectory();
        }

        // Empty files and folders have no stream.
        for (UInt32 i = 0; i < db.NumFiles; ++i)
        {
            if (db.FileToFolder[i] != (UInt32) -1)
                continue;

            const auto dest = safeDestination (root, fileName (db, i));

            if (dest == juce::File())
                continue;

            if (SzArEx_IsDir (&db, i))
                dest.createDirectory();
            else
                dest.create();
        }

        return juce::Result::ok();
    }
}

bool isArchive (const juce::File& file)
{
    return file.hasFileExtension ("zip;tar;7z");
}

juce::Result extract (const juce::File& archive, const juce::File& destination, const Progress& progress)
{
    if (const auto r = destination.createDirectory(); r.failed())
        return r;

    if (archive.hasFileExtension ("zip"))
        return extractZip (archive, destination, progress);

    if (archive.hasFileExtension ("7z"))
        return extract7z (archive, destination, progress);

    if (archive.hasFileExtension ("tar"))
    {
        juce::FileInputStream in (archive);

        if (in.failedToOpen())
            return juce::Result::fail ("Can't open " + archive.getFullPathName());

        const auto length = (double) std::max<std::int64_t> (1, in.getTotalLength());
        return extractTar (in, destination, [&in, length] { return (double) in.getPosition() / length; }, progress);
    }

    return juce::Result::fail ("Lithium Drums can't unpack " + archive.getFileName());
}

} // namespace lithium::packs
