#include <chrono>
#include <cstdint>
#include <optional>
#include <thread>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include <osg/GL>
#include <osg/Image>
#include <osg/ref_ptr>

#include <apps/components_tests/rtx/support/image.hpp>
#include <components/rtx/common/bufferpool.hpp>
#include <components/rtx/image/textureencoding.hpp>
#include <components/rtx/preprocess/texturefeed.hpp>
#include <components/rtx/preprocess/texturehands.hpp>

namespace Rtx
{
    namespace
    {
        /// A request for `file` read as colour, from level `first` to its last, under `ticket`.
        FeedRequest requestOf(
            const osg::ref_ptr<const osg::Image>& file, const std::uint32_t ticket, const std::uint8_t first)
        {
            const ImageShape shape = shapeOfFile(*file, nullptr, TextureEncoding::Colour);
            return FeedRequest{ .mTicket = FeedTicket{ ticket },
                .mFile = file,
                .mEncoding = TextureEncoding::Colour,
                .mShape = shape,
                .mLevels = LevelSpan{ .mFirst = first, .mEnd = shape.mLevels } };
        }

        /// Every completion the hands have, in the order taken.
        std::vector<FeedArrival> takeAll(TextureHands& hands)
        {
            std::vector<FeedArrival> taken;
            while (std::optional<FeedArrival> arrival = hands.take())
                taken.push_back(std::move(*arrival));
            return taken;
        }

        /// **A loose file's chain is made in BC7 and handed over from the first level asked, in the order asked**: an 8
        /// by 8 loose colour file is planned as BC7 of four levels, 64 + 16 + 16 + 16 bytes; asked from level 0 its
        /// arrival holds all 112, and from level 2 the 32 of levels 2 and 3, level 2 at offset nought. Asked after it,
        /// a file in a format no reader here decodes fails, saying why, and arrives after the others.
        TEST(RtxTextureHandsTest, aChainIsHandedOverFromTheFirstLevelAskedInTheOrderAsked)
        {
            BufferPool pool(TextureHands::sPoolBytes);
            TextureHands hands(nullptr, pool, 2, nullptr);
            const osg::ref_ptr<const osg::Image> loose = Testing::makeImage("textures/loose.dds", 8, 8);
            const osg::ref_ptr<const osg::Image> odd
                = Testing::makeImage("textures/odd.dds", 2, 2, GL_RGB, GL_UNSIGNED_BYTE_3_3_2);
            ASSERT_TRUE(hands.hasCredit());
            hands.ask(requestOf(loose, 1, 0));
            hands.ask(requestOf(loose, 2, 2));
            hands.ask(requestOf(odd, 3, 0));
            hands.flush();

            const std::vector<FeedArrival> taken = takeAll(hands);
            ASSERT_EQ(taken.size(), 3u);
            EXPECT_EQ(taken[0].mRequest.mTicket, FeedTicket{ 1 });
            EXPECT_EQ(taken[1].mRequest.mTicket, FeedTicket{ 2 });
            EXPECT_EQ(taken[2].mRequest.mTicket, FeedTicket{ 3 });

            const auto* whole = std::get_if<FeedMade>(&taken[0].mOutcome);
            ASSERT_NE(whole, nullptr);
            EXPECT_EQ(whole->mShape, taken[0].mRequest.mShape);
            EXPECT_EQ(whole->mTexels.mBytes.size(), 112u);
            ASSERT_EQ(whole->mLevels.size(), 4u);
            EXPECT_EQ(whole->mLevels[1].mOffset, 64u);

            const auto* coarse = std::get_if<FeedMade>(&taken[1].mOutcome);
            ASSERT_NE(coarse, nullptr);
            EXPECT_EQ(coarse->mTexels.mBytes.size(), 32u) << "levels 2 and 3 alone";
            ASSERT_EQ(coarse->mLevels.size(), 4u);
            EXPECT_EQ(coarse->mLevels[2].mOffset, 0u);
            EXPECT_EQ(coarse->mLevels[3].mOffset, 16u);

            const auto* refused = std::get_if<FeedFailed>(&taken[2].mOutcome);
            ASSERT_NE(refused, nullptr);
            EXPECT_FALSE(refused->mWhy.empty());
            EXPECT_TRUE(hands.isIdle());
        }

        /// **A request forgotten never arrives, and the hands go with requests in flight**: of three asked, the second
        /// forgotten at once arrives never, made or not, and the other two in their order; hands asked a dozen and let
        /// go before any is taken stop and drop them.
        TEST(RtxTextureHandsTest, aForgottenRequestNeverArrivesAndTheHandsGoWithRequestsInFlight)
        {
            BufferPool pool(TextureHands::sPoolBytes);
            const osg::ref_ptr<const osg::Image> loose = Testing::makeImage("textures/loose.dds", 64, 64);
            {
                TextureHands hands(nullptr, pool, 1, nullptr);
                hands.ask(requestOf(loose, 1, 0));
                hands.ask(requestOf(loose, 2, 0));
                hands.forget(FeedTicket{ 2 });
                hands.ask(requestOf(loose, 3, 0));
                hands.flush();
                const std::vector<FeedArrival> taken = takeAll(hands);
                ASSERT_EQ(taken.size(), 2u);
                EXPECT_EQ(taken[0].mRequest.mTicket, FeedTicket{ 1 });
                EXPECT_EQ(taken[1].mRequest.mTicket, FeedTicket{ 3 });
            }
            {
                TextureHands going(nullptr, pool, 2, nullptr);
                for (std::uint32_t at = 0; at < 12; ++at)
                    going.ask(requestOf(loose, 10 + at, 0));
            }
            EXPECT_EQ(pool.getHeld().mInUse, 0u) << "a buffer a dropped request held";
        }

        /// **A slow head job completes without deadlocking when fast jobs behind it fill the pool**, and forgetting a
        /// completed job immediately releases its pooled buffer without waiting for `take()`.
        TEST(RtxTextureHandsTest, slowHeadCompletesWhenLaterJobsFillPoolAndForgetFreesCompletedBuffer)
        {
            constexpr std::size_t sSmallPoolBytes = std::size_t{ 128 } << 10;
            {
                BufferPool pool(sSmallPoolBytes);
                TextureHands hands(nullptr, pool, 4, nullptr);
                const osg::ref_ptr<const osg::Image> slowHead = Testing::makeImage("textures/slow_head.dds", 512, 512);
                const osg::ref_ptr<const osg::Image> fastTail = Testing::makeImage("textures/fast_tail.dds", 8, 8);

                hands.ask(requestOf(slowHead, 1, 0));
                for (std::uint32_t ticket = 2; ticket <= 9; ++ticket)
                    hands.ask(requestOf(fastTail, ticket, 0));

                std::vector<FeedArrival> taken;
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                while (taken.size() < 9 && std::chrono::steady_clock::now() < deadline)
                {
                    if (std::optional<FeedArrival> arrival = hands.take())
                        taken.push_back(std::move(*arrival));
                    else
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }

                ASSERT_EQ(taken.size(), 9u) << "FIFO head deadlocked waiting for BufferPool room";
                for (std::uint32_t i = 0; i < 9; ++i)
                {
                    EXPECT_EQ(taken[i].mRequest.mTicket, FeedTicket{ i + 1 });
                    EXPECT_TRUE(std::holds_alternative<FeedMade>(taken[i].mOutcome));
                }
                taken.clear();
                EXPECT_EQ(pool.getHeld().mInUse, 0u);
            }

            {
                BufferPool pool(sSmallPoolBytes);
                TextureHands hands(nullptr, pool, 1, nullptr);
                const osg::ref_ptr<const osg::Image> loose = Testing::makeImage("textures/loose_forget.dds", 8, 8);
                hands.ask(requestOf(loose, 1, 0));
                hands.ask(requestOf(loose, 2, 0));
                hands.flush();
                EXPECT_EQ(pool.getHeld().mInUse, std::size_t{ 128 } << 10);

                hands.forget(FeedTicket{ 2 });
                EXPECT_EQ(pool.getHeld().mInUse, std::size_t{ 64 } << 10)
                    << "forgetting a completed job must release its pooled buffer immediately";

                std::vector<FeedArrival> taken = takeAll(hands);
                ASSERT_EQ(taken.size(), 1u);
                EXPECT_EQ(taken[0].mRequest.mTicket, FeedTicket{ 1 });
                taken.clear();
                EXPECT_EQ(pool.getHeld().mInUse, 0u);
            }
        }
    }
}
