/**
 * @file tests/commands_test.cpp
 * @brief Mega SDK unit tests for commands
 *
 * (c) 2019 by Mega Limited, Wellsford, New Zealand
 *
 * This file is part of the MEGA SDK - Client Access Engine.
 *
 * Applications using the MEGA API must present a valid application key
 * and comply with the the rules set forth in the Terms of Service.
 *
 * The MEGA SDK is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * @copyright Simplified (2-clause) BSD License.
 *
 * You should have received a copy of the license along with this
 * program.
 */

// Note: The tests in this module are meant to be pure unit tests: Fast tests without I/O.

#include <memory>

#include <gtest/gtest.h>

#include <mega/sync.h>
#include <mega/megaapp.h>
#include <mega/megaclient.h>
#include <mega/types.h>

#include "constants.h"
#include "FsNode.h"
#include "DefaultedDirAccess.h"
#include "DefaultedFileAccess.h"
#include "DefaultedFileSystemAccess.h"
#include "utils.h"

namespace {

class MockApp : public mega::MegaApp
{
public:
    MockApp() = default;
    explicit MockApp(const bool syncable)
    : mSyncable{syncable}
    {}

    bool sync_syncable(mega::Sync*, const char*, std::string*) override
    {
        return mSyncable;
    }
private:
    bool mSyncable = true;
};

class MockFileAccess : public mt::DefaultedFileAccess
{
public:
    explicit MockFileAccess(std::map<std::string, const mt::FsNode*>& fsNodes)
    : mFsNodes{fsNodes}
    {
        fsidvalid = true;
    }

    bool fopen(std::string* path, bool, bool) override
    {
        const auto fsNodePair = mFsNodes.find(*path);
        if (fsNodePair != mFsNodes.end())
        {
            mCurrentFsNode = fsNodePair->second;
            fsid = mCurrentFsNode->getFsId();
            size = mCurrentFsNode->getSize();
            mtime = mCurrentFsNode->getMTime();
            type = mCurrentFsNode->getType();
            return true;
        }
        else
        {
            return false;
        }
    }

    bool frawread(mega::byte* buffer, unsigned size, m_off_t) override
    {
        for (unsigned i = 0; i < size; ++i)
        {
            assert(i < mCurrentFsNode->getContent().size());
            buffer[i] = mCurrentFsNode->getContent()[i];
        }
        return true;
    }

private:
    const mt::FsNode* mCurrentFsNode{};
    std::map<std::string, const mt::FsNode*>& mFsNodes;
};

class MockDirAccess : public mt::DefaultedDirAccess
{
public:
    explicit MockDirAccess(std::map<std::string, const mt::FsNode*>& fsNodes)
    : mFsNodes{fsNodes}
    {}

    bool dopen(std::string* path, mega::FileAccess* fa, bool) override
    {
        assert(fa->type == mega::FOLDERNODE);
        const auto fsNodePair = mFsNodes.find(*path);
        if (fsNodePair != mFsNodes.end())
        {
            mCurrentFsNode = fsNodePair->second;
            return true;
        }
        else
        {
            return false;
        }
    }

    bool dnext(std::string* localpath, std::string* localname, bool = true, mega::nodetype_t* = NULL) override
    {
        assert(mCurrentFsNode);
        assert(mCurrentFsNode->getPath() == *localpath);
        const auto& children = mCurrentFsNode->getChildren();
        if (mCurrentChildIndex < children.size())
        {
            *localname = children[mCurrentChildIndex]->getName();
            ++mCurrentChildIndex;
            return true;
        }
        else
        {
            mCurrentChildIndex = 0;
            mCurrentFsNode = nullptr;
            return false;
        }
    }

private:
    const mt::FsNode* mCurrentFsNode{};
    std::size_t mCurrentChildIndex{};
    std::map<std::string, const mt::FsNode*>& mFsNodes;
};

class MockFileSystemAccess : public mt::DefaultedFileSystemAccess
{
public:
    explicit MockFileSystemAccess(std::map<std::string, const mt::FsNode*>& fsNodes)
    : mFsNodes{fsNodes}
    {}

    mega::FileAccess* newfileaccess() override
    {
        return new MockFileAccess{mFsNodes};
    }

    mega::DirAccess* newdiraccess() override
    {
        return new MockDirAccess{mFsNodes};
    }

    void local2path(std::string* local, std::string* path) const override
    {
        *path = *local;
    }

private:
    std::map<std::string, const mt::FsNode*>& mFsNodes;
};

struct Fixture
{
    explicit Fixture(std::string localname)
    : mSync{mt::makeSync(std::move(localname), mLocalNodes)}
    {}

    MEGA_DISABLE_COPY_MOVE(Fixture)

    MockApp mApp;
    std::map<std::string, const mt::FsNode*> mFsNodes;
    mega::handlelocalnode_map mLocalNodes;
    MockFileSystemAccess mFsAccess{mFsNodes};
    std::unique_ptr<mega::Sync> mSync;
};

}

TEST(Sync, isPathSyncable)
{
    ASSERT_TRUE(mega::isPathSyncable("dir/foo", "dir/foo" + mt::gLocalDebris, "/"));
    ASSERT_FALSE(mega::isPathSyncable("dir/foo" + mt::gLocalDebris, "dir/foo" + mt::gLocalDebris, "/"));
    ASSERT_TRUE(mega::isPathSyncable(mt::gLocalDebris + "bar", mt::gLocalDebris, "/"));
    ASSERT_FALSE(mega::isPathSyncable(mt::gLocalDebris + "/", mt::gLocalDebris, "/"));
}

TEST(Sync, invalidateFilesystemIds)
{
    Fixture fx{"d"};

    // Level 0
    mega::LocalNode& d = fx.mSync->localroot;

    // Level 1
    auto d_0 = mt::makeLocalNode(*fx.mSync, d, mega::FOLDERNODE, "d_0");
    auto f_0 = mt::makeLocalNode(*fx.mSync, d, mega::FILENODE, "f_0");

    mt::collectAllLocalNodes(fx.mLocalNodes, d);

    mega::invalidateFilesystemIds(fx.mLocalNodes, d);

    ASSERT_TRUE(fx.mLocalNodes.empty());
    ASSERT_EQ(fx.mLocalNodes.end(), d.fsid_it);
    ASSERT_EQ(fx.mLocalNodes.end(), d_0->fsid_it);
    ASSERT_EQ(fx.mLocalNodes.end(), f_0->fsid_it);
    ASSERT_EQ(mega::UNDEF, d.fsid);
    ASSERT_EQ(mega::UNDEF, d_0->fsid);
    ASSERT_EQ(mega::UNDEF, f_0->fsid);
}

TEST(Sync, assignFilesystemIds_whenFilesystemMatchesLocalNodes)
{
    Fixture fx{"d"};

    // Level 0
    mt::FsNode d{nullptr, mega::FOLDERNODE, "d"};
    mega::LocalNode& ld = fx.mSync->localroot;

    // Level 1
    mt::FsNode d_0{&d, mega::FOLDERNODE, "d_0"};
    auto ld_0 = mt::makeLocalNode(*fx.mSync, ld, mega::FOLDERNODE, "d_0");
    mt::FsNode d_1{&d, mega::FOLDERNODE, "d_1"};
    auto ld_1 = mt::makeLocalNode(*fx.mSync, ld, mega::FOLDERNODE, "d_1");
    mt::FsNode f_2{&d, mega::FILENODE, "f_2"};
    auto lf_2 = mt::makeLocalNode(*fx.mSync, ld, mega::FILENODE, "f_2", f_2.getFingerprint());

    // Level 2
    mt::FsNode f_0_0{&d_0, mega::FILENODE, "f_0_0"};
    auto lf_0_0 = mt::makeLocalNode(*fx.mSync, *ld_0, mega::FILENODE, "f_0_0", f_0_0.getFingerprint());
    mt::FsNode f_0_1{&d_0, mega::FILENODE, "f_0_1"};
    auto lf_0_1 = mt::makeLocalNode(*fx.mSync, *ld_0, mega::FILENODE, "f_0_1", f_0_1.getFingerprint());
    mt::FsNode f_1_0{&d_1, mega::FILENODE, "f_1_0"};
    auto lf_1_0 = mt::makeLocalNode(*fx.mSync, *ld_1, mega::FILENODE, "f_1_0", f_1_0.getFingerprint());
    mt::FsNode d_1_1{&d_1, mega::FOLDERNODE, "d_1_1"};
    auto ld_1_1 = mt::makeLocalNode(*fx.mSync, *ld_1, mega::FOLDERNODE, "d_1_1");

    // Level 3
    mt::FsNode f_1_1_0{&d_1_1, mega::FILENODE, "f_1_1_0"};
    auto lf_1_1_0 = mt::makeLocalNode(*fx.mSync, *ld_1_1, mega::FILENODE, "f_1_1_0", f_1_1_0.getFingerprint());

    mt::collectAllFsNodes(fx.mFsNodes, d);

    mt::collectAllLocalNodes(fx.mLocalNodes, ld);

    const auto success = mega::assignFilesystemIds(*fx.mSync, fx.mApp, fx.mFsAccess, fx.mLocalNodes,
                                                   mt::gLocalDebris, "/", true);

    ASSERT_TRUE(success);

    // assert that directores have invalid fs IDs
    ASSERT_EQ(mega::UNDEF, ld_0->fsid);
    ASSERT_EQ(mega::UNDEF, ld_1->fsid);
    ASSERT_EQ(mega::UNDEF, ld_1_1->fsid);

    // assert that all file `LocalNode`s have same fs IDs as the corresponding `FsNode`s
    ASSERT_EQ(f_2.getFsId(), lf_2->fsid);
    ASSERT_EQ(f_0_0.getFsId(), lf_0_0->fsid);
    ASSERT_EQ(f_0_1.getFsId(), lf_0_1->fsid);
    ASSERT_EQ(f_1_0.getFsId(), lf_1_0->fsid);
    ASSERT_EQ(f_1_1_0.getFsId(), lf_1_1_0->fsid);

    // assert that the local node map is correct
    constexpr std::size_t fileCount = 5;
    ASSERT_EQ(fileCount, fx.mLocalNodes.size());
    ASSERT_EQ(lf_2->fsid_it, fx.mLocalNodes.find(lf_2->fsid));
    ASSERT_EQ(lf_2.get(), fx.mLocalNodes[lf_2->fsid]);
    ASSERT_EQ(lf_0_0->fsid_it, fx.mLocalNodes.find(lf_0_0->fsid));
    ASSERT_EQ(lf_0_0.get(), fx.mLocalNodes[lf_0_0->fsid]);
    ASSERT_EQ(lf_0_1->fsid_it, fx.mLocalNodes.find(lf_0_1->fsid));
    ASSERT_EQ(lf_0_1.get(), fx.mLocalNodes[lf_0_1->fsid]);
    ASSERT_EQ(lf_1_0->fsid_it, fx.mLocalNodes.find(lf_1_0->fsid));
    ASSERT_EQ(lf_1_0.get(), fx.mLocalNodes[lf_1_0->fsid]);
    ASSERT_EQ(lf_1_1_0->fsid_it, fx.mLocalNodes.find(lf_1_1_0->fsid));
    ASSERT_EQ(lf_1_1_0.get(), fx.mLocalNodes[lf_1_1_0->fsid]);
}
