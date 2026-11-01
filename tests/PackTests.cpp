#include "ArchiveFixtures.h"
#include "packs/Archive.h"
#include "packs/PackInstaller.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace lithium
{

namespace
{
    std::uint32_t crc32 (const juce::MemoryBlock& data)
    {
        std::uint32_t crc = 0xffffffff;

        for (size_t i = 0; i < data.getSize(); ++i)
        {
            crc ^= (std::uint8_t) data[i];

            for (int bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }

        return ~crc;
    }

    /** A zip of stored (uncompressed) entries, written by hand so a test can
        make Zip64 records, or entries no zip tool would write. */
    juce::MemoryBlock makeStoredZip (const std::vector<std::pair<juce::String, juce::String>>& entries, bool zip64, bool corrupt = false)
    {
        juce::MemoryOutputStream out, directory;

        auto u16 = [] (juce::OutputStream& s, int v)              { s.writeShort ((short) v); };
        auto u32 = [] (juce::OutputStream& s, std::uint32_t v)    { s.writeInt ((int) v); };
        auto u64 = [] (juce::OutputStream& s, std::uint64_t v)    { s.writeInt64 ((juce::int64) v); };

        for (const auto& [name, text] : entries)
        {
            juce::MemoryBlock data (text.toRawUTF8(), text.getNumBytesAsUTF8());
            const auto crc = crc32 (data) ^ (corrupt ? 1u : 0u);
            const auto offset = (std::uint64_t) out.getPosition();
            const auto size = (std::uint32_t) data.getSize();
            const auto nameBytes = (int) name.getNumBytesAsUTF8();

            u32 (out, 0x04034b50); u16 (out, 45); u16 (out, 0x800); u16 (out, 0); u16 (out, 0); u16 (out, 0); u32 (out, crc);
            u32 (out, zip64 ? 0xffffffff : size); u32 (out, zip64 ? 0xffffffff : size); u16 (out, nameBytes); u16 (out, zip64 ? 20 : 0);
            out.write (name.toRawUTF8(), (size_t) nameBytes);

            if (zip64)
            {
                u16 (out, 1); u16 (out, 16); u64 (out, size); u64 (out, size);
            }

            out << data;

            u32 (directory, 0x02014b50); u16 (directory, 3 << 8 | 45); u16 (directory, 45); u16 (directory, 0x800); u16 (directory, 0);
            u16 (directory, 0); u16 (directory, 0); u32 (directory, crc);
            u32 (directory, zip64 ? 0xffffffff : size); u32 (directory, zip64 ? 0xffffffff : size);
            u16 (directory, nameBytes); u16 (directory, zip64 ? 28 : 0); u16 (directory, 0); u16 (directory, 0); u16 (directory, 0);
            u32 (directory, 0100644u << 16); u32 (directory, zip64 ? 0xffffffff : (std::uint32_t) offset);
            directory.write (name.toRawUTF8(), (size_t) nameBytes);

            if (zip64)
            {
                u16 (directory, 1); u16 (directory, 24); u64 (directory, size); u64 (directory, size); u64 (directory, offset);
            }
        }

        const auto directoryOffset = (std::uint64_t) out.getPosition();
        out << directory.getMemoryBlock();
        const auto count = (std::uint64_t) entries.size();

        if (zip64)
        {
            const auto recordOffset = (std::uint64_t) out.getPosition();
            u32 (out, 0x06064b50); u64 (out, 44); u16 (out, 45); u16 (out, 45); u32 (out, 0); u32 (out, 0);
            u64 (out, count); u64 (out, count); u64 (out, directory.getDataSize()); u64 (out, directoryOffset);
            u32 (out, 0x07064b50); u32 (out, 0); u64 (out, recordOffset); u32 (out, 1);
        }

        u32 (out, 0x06054b50); u16 (out, 0); u16 (out, 0);
        u16 (out, zip64 ? 0xffff : (int) count); u16 (out, zip64 ? 0xffff : (int) count);
        u32 (out, zip64 ? 0xffffffff : (std::uint32_t) directory.getDataSize());
        u32 (out, zip64 ? 0xffffffff : (std::uint32_t) directoryOffset);
        u16 (out, 0);

        return out.getMemoryBlock();
    }

    /** A tar, with a GNU long name, a file that tries to climb out, and a symlink. */
    juce::MemoryBlock makeTar()
    {
        juce::MemoryOutputStream out;

        auto header = [&out] (const juce::String& entryName, std::int64_t size, char type, const juce::String& linkTarget = {})
        {
            char h[512] {};
            entryName.copyToUTF8 (h, 100);
            juce::String ("0000644").copyToUTF8 (h + 100, 8);
            juce::String octal;

            for (auto v = size; v > 0; v /= 8)
                octal = juce::String ((int) (v % 8)) + octal;

            octal.paddedLeft ('0', 11).copyToUTF8 (h + 124, 12);
            h[156] = type;
            linkTarget.copyToUTF8 (h + 157, 100);
            juce::String ("ustar  ").copyToUTF8 (h + 257, 8);
            out.write (h, 512);
        };

        auto data = [&out] (const juce::String& text)
        {
            out.write (text.toRawUTF8(), text.getNumBytesAsUTF8());
            const auto pad = (512 - (int) (text.getNumBytesAsUTF8() % 512)) % 512;
            out.writeRepeatedByte (0, (size_t) pad);
        };

        const auto longName = "pack/" + juce::String::repeatedString ("long", 40) + ".txt";
        header ("././@LongLink", (std::int64_t) longName.getNumBytesAsUTF8() + 1, 'L');
        data (longName + juce::String::charToString (0));
        header ("pack/short-name", 10, '0');
        data ("long named");   // the long name applies to this entry

        header ("pack/", 0, '5');
        header ("pack/drums.txt", 5, '0');
        data ("drums");
        header ("../outside.txt", 4, '0');
        data ("evil");
        header ("pack/link", 0, '2', "/etc/passwd");

        out.writeRepeatedByte (0, 1024);
        return out.getMemoryBlock();
    }
}

class ArchiveTests final : public juce::UnitTest
{
public:
    ArchiveTests() : juce::UnitTest ("Archives", "Lithium") {}

    void runTest() override
    {
        const juce::TemporaryFile temp;
        const auto root = temp.getFile();
        root.createDirectory();

        auto write = [&root] (const juce::String& fileName, const void* data, size_t size)
        {
            const auto f = root.getChildFile (fileName);
            f.replaceWithData (data, size);
            return f;
        };

        beginTest ("Zip: deflated entries, with their checksums checked");
        {
            // Made by JUCE's own zip writer, which deflates.
            const auto source = root.getChildFile ("zip-source");
            source.getChildFile ("kit/audio/hit.wav").create();
            source.getChildFile ("kit/audio/hit.wav").replaceWithText (juce::String::repeatedString ("x", 50000));
            source.getChildFile ("kit/readme.txt").replaceWithText ("hello lithium");

            juce::ZipFile::Builder builder;
            builder.addFile (source.getChildFile ("kit/audio/hit.wav"), 9, "kit/audio/hit.wav");
            builder.addFile (source.getChildFile ("kit/readme.txt"), 0, "kit/readme.txt");
            const auto zip = root.getChildFile ("deflated.zip");

            {
                juce::FileOutputStream out (zip);
                builder.writeToStream (out, nullptr);
            }

            double lastProgress = -1.0;
            const auto r = packs::extract (zip, root.getChildFile ("deflated"), [&] (double p) { lastProgress = p; return true; });
            expect (r.wasOk(), r.getErrorMessage());
            expectEquals (root.getChildFile ("deflated/kit/audio/hit.wav").getSize(), (juce::int64) 50000);
            expectEquals (root.getChildFile ("deflated/kit/readme.txt").loadFileAsString(), juce::String ("hello lithium"));
            expectWithinAbsoluteError (lastProgress, 1.0, 0.001);
        }

        beginTest ("Zip64: sizes and offsets in the Zip64 records");
        {
            const auto zip = makeStoredZip ({ { "big/one.txt", "first" }, { "big/two.txt", "second" } }, true);
            const auto file = write ("zip64.zip", zip.getData(), zip.getSize());
            const auto r = packs::extract (file, root.getChildFile ("zip64"));
            expect (r.wasOk(), r.getErrorMessage());
            expectEquals (root.getChildFile ("zip64/big/two.txt").loadFileAsString(), juce::String ("second"));
        }

        beginTest ("Zip: entries can't land outside the folder, and damage is caught");
        {
            const auto zip = makeStoredZip ({ { "../escaped.txt", "x" }, { "/etc/escaped.txt", "x" }, { "fine/ok.txt", "ok" } }, false);
            const auto r = packs::extract (write ("evil.zip", zip.getData(), zip.getSize()), root.getChildFile ("evil"));
            expect (r.wasOk(), r.getErrorMessage());
            expect (! root.getChildFile ("escaped.txt").exists());
            expect (! root.getChildFile ("evil/etc/escaped.txt").exists());
            expect (root.getChildFile ("evil/fine/ok.txt").existsAsFile());

            const auto damaged = makeStoredZip ({ { "a.txt", "some data" } }, false, true);
            const auto bad = packs::extract (write ("damaged.zip", damaged.getData(), damaged.getSize()), root.getChildFile ("damaged"));
            expect (bad.failed() && bad.getErrorMessage().contains ("damaged"), bad.getErrorMessage());

            const auto garbage = juce::String ("not a zip at all").toStdString();
            expect (packs::extract (write ("garbage.zip", garbage.data(), garbage.size()), root.getChildFile ("garbage")).failed());
        }

        beginTest ("Tar: long names, folders, and no climbing out or links");
        {
            const auto tar = makeTar();
            const auto r = packs::extract (write ("pack.tar", tar.getData(), tar.getSize()), root.getChildFile ("tar"));
            expect (r.wasOk(), r.getErrorMessage());
            expectEquals (root.getChildFile ("tar/pack/drums.txt").loadFileAsString(), juce::String ("drums"));
            expectEquals (root.getChildFile ("tar/pack/" + juce::String::repeatedString ("long", 40) + ".txt").loadFileAsString(),
                          juce::String ("long named"));
            expect (! root.getChildFile ("tar/pack/short-name").exists());
            expect (! root.getChildFile ("outside.txt").exists());
            expect (! root.getChildFile ("tar/pack/link").exists());
        }

        beginTest ("7z: a .tar.7z (LZMA) and a plain 7z (LZMA2), decoded as they're read");
        {
            for (const auto& [archiveName, data, size] : { std::tuple { "kit.tar.7z", test::kitTar7z, sizeof (test::kitTar7z) },
                                                           std::tuple { "plain.7z", test::plain7z, sizeof (test::plain7z) } })
            {
                const auto dest = root.getChildFile (juce::String (archiveName) + "-out");
                const auto r = packs::extract (write (archiveName, data, size), dest);
                expect (r.wasOk(), r.getErrorMessage());
                expectEquals (dest.getChildFile ("kit/readme.txt").loadFileAsString(), juce::String ("hello lithium\n"), archiveName);
                expectEquals (dest.getChildFile ("kit/audio/hit.wav").getSize(), (juce::int64) 3000, archiveName);
                expect (! dest.getChildFile ("kit.tar").exists(), "the tar is unpacked, not written out");
            }
        }

        beginTest ("Cancelling stops unpacking");
        {
            const auto zip = makeStoredZip ({ { "a.txt", "a" }, { "b.txt", "b" } }, false);
            const auto r = packs::extract (write ("cancel.zip", zip.getData(), zip.getSize()), root.getChildFile ("cancel"), [] (double) { return false; });
            expect (r.failed() && r.getErrorMessage() == "Cancelled");
        }
    }
};

//==============================================================================
class PackInstallerTests final : public juce::UnitTest
{
public:
    PackInstallerTests() : juce::UnitTest ("Pack installer", "Lithium") {}

    void runTest() override
    {
        beginTest ("The built-in catalog lists every pack with downloads, sizes and a licence");
        {
            expect (packs::catalog().size() >= 10);

            for (const auto& p : packs::catalog())
            {
                expect (p.id.isNotEmpty() && p.name.isNotEmpty() && p.licenseName.isNotEmpty(), p.id);
                expect (! p.downloads.empty() && p.downloadSize() > 0 && p.unpackedSize > 0, p.id);
                expect (p.type == packs::CatalogPack::Type::grooves || ! p.kits.isEmpty(), p.id);

                for (const auto& d : p.downloads)
                    expect (d.url.startsWith ("http") && d.file.isNotEmpty() && ! d.file.contains ("/"), p.id + ": " + d.file);
            }

            expect (packs::findPack ("drumgizmo-drskit") != nullptr);
            expectEquals (packs::formatSize (2803397710), juce::String ("2.8 GB"));
            expectEquals (packs::formatSize (3260318), juce::String ("3 MB"));
        }

        beginTest ("A pack downloads (from a file URL), unpacks, imports and cleans up after itself");

        const juce::TemporaryFile temp;
        const auto root = temp.getFile();
        const auto kits = root.getChildFile ("Kits"), groovesFolder = root.getChildFile ("Grooves");

        // A tiny stand-in for the Groove MIDI Dataset's zip.
        const auto source = root.getChildFile ("source/groove");
        source.getChildFile ("drummer1/session1").createDirectory();
        source.getChildFile ("LICENSE").replaceWithText ("Creative Commons Attribution 4.0 International License");
        source.getChildFile ("info.csv").replaceWithText (
            "drummer,session,id,style,bpm,beat_type,time_signature,midi_filename,audio_filename,duration,split\n"
            "drummer1,drummer1/session1,drummer1/session1/1,rock,120,beat,4-4,drummer1/session1/1_rock_120_beat_4-4.mid,x.wav,8,train\n");

        {
            juce::MidiMessageSequence track;
            track.addEvent (juce::MidiMessage::noteOn (10, 36, (juce::uint8) 100), 0.0);
            track.addEvent (juce::MidiMessage::noteOn (10, 38, (juce::uint8) 100), 480.0 * 5);
            juce::MidiFile midi;
            midi.setTicksPerQuarterNote (480);
            midi.addTrack (track);
            juce::FileOutputStream out (source.getChildFile ("drummer1/session1/1_rock_120_beat_4-4.mid"));
            midi.writeTo (out);
        }

        const auto zip = root.getChildFile ("server/grooves.zip");
        zip.getParentDirectory().createDirectory();

        {
            juce::ZipFile::Builder builder;

            for (const auto& f : source.findChildFiles (juce::File::findFiles, true))
                builder.addFile (f, 6, "groove/" + f.getRelativePathFrom (source).replaceCharacter ('\\', '/'));

            juce::FileOutputStream out (zip);
            builder.writeToStream (out, nullptr);
        }

        packs::CatalogPack pack;
        pack.id = "test-grooves";
        pack.name = "Test grooves";
        pack.type = packs::CatalogPack::Type::grooves;
        pack.downloads.push_back ({ juce::URL (zip).toString (false), "grooves.zip", zip.getSize(), 0 });
        pack.unpackedSize = 10000;

        juce::StringArray log;
        const auto r = packs::Installer::installNow (pack, kits, groovesFolder, [&log] (const juce::String& s) { log.add (s); });
        expect (r.wasOk(), r.getErrorMessage() + "\n" + log.joinIntoString ("\n"));
        expect (groovesFolder.getChildFile ("Groove MIDI Dataset/grooves.json").existsAsFile());
        expect (groovesFolder.getChildFile ("Groove MIDI Dataset/LICENSE.txt").existsAsFile());
        expect (! packs::Installer::workFolder (kits, pack.id).exists(), "the download and unpacked files are gone");
        expect (! kits.getChildFile (".downloads").exists());
        expect (zip.existsAsFile(), "the source is untouched");

        beginTest ("A download that comes up short fails, and can carry on later");
        {
            auto shortPack = pack;
            shortPack.id = "test-short";
            shortPack.downloads[0].size = zip.getSize() + 100;   // more than the server has
            const auto failed = packs::Installer::installNow (shortPack, kits, groovesFolder, [] (const juce::String&) {});
            expect (failed.failed() && failed.getErrorMessage().contains ("stopped early"), failed.getErrorMessage());
            expect (packs::Installer::workFolder (kits, shortPack.id).getChildFile ("grooves.zip.part").existsAsFile(),
                    "the partial download stays, to resume from");
        }

        beginTest ("Not enough disk space is caught before downloading");
        {
            auto huge = pack;
            huge.id = "test-huge";
            huge.unpackedSize = std::numeric_limits<std::int64_t>::max() / 4;
            const auto failed = packs::Installer::installNow (huge, kits, groovesFolder, [] (const juce::String&) {});
            expect (failed.failed() && failed.getErrorMessage().contains ("disk space"), failed.getErrorMessage());
        }

        beginTest ("The background installer reports unknown packs as failed");
        {
            juce::SharedResourcePointer<packs::Installer> installer;
            installer->install ("no-such-pack", kits, groovesFolder);

            for (int i = 0; i < 200 && installer->isBusy(); ++i)
                juce::Thread::sleep (10);

            const auto status = installer->getStatus ("no-such-pack");
            expect (status.has_value() && status->phase == packs::Installer::Phase::failed);
        }
    }
};

static ArchiveTests archiveTests;
static PackInstallerTests packInstallerTests;

} // namespace lithium
