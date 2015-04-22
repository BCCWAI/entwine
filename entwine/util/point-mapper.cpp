/******************************************************************************
* Copyright (c) 2016, Connor Manning (connor@hobu.co)
*
* Entwine -- Point cloud indexing
*
* Entwine is available under the terms of the LGPL2 license. See COPYING
* for specific license text and more information.
*
******************************************************************************/

#include <entwine/util/point-mapper.hpp>

#include <sys/mman.h>

#include <cassert>
#include <cstring>
#include <iostream>

#include <entwine/compression/util.hpp>
#include <entwine/http/s3.hpp>
#include <entwine/tree/point-info.hpp>
#include <entwine/tree/branches/clipper.hpp>
#include <entwine/types/linking-point-view.hpp>
#include <entwine/types/schema.hpp>
#include <entwine/types/single-point-table.hpp>
#include <entwine/util/fs.hpp>
#include <entwine/util/pool.hpp>
#include <entwine/util/platform.hpp>

namespace
{
    const std::size_t pointsPerSlot(entwine::platform::pageSize());
}

namespace entwine
{
namespace fs
{

Slot::Slot(
        const Schema& schema,
        const FileDescriptor& fd,
        const std::size_t firstPoint)
    : m_schema(schema)
    , m_mapping(0)
    , m_data()
    , m_points(pointsPerSlot, std::atomic<const Point*>(0))
    , m_locks(pointsPerSlot)
{
    const std::size_t pointSize(m_schema.pointSize());
    const std::size_t dataSize(pointsPerSlot * pointSize);

    m_mapping =
            static_cast<char*>(mmap(
                0,
                dataSize,
                PROT_READ | PROT_WRITE,
                MAP_SHARED,
                fd.id(),
                firstPoint * pointSize));

    if (m_mapping == MAP_FAILED)
    {
        std::cout << "Mapping failed: " << strerror(errno) << std::endl;
        throw std::runtime_error("Could not create mapping!");
    }

    m_data.resize(dataSize);
    std::memcpy(m_data.data(), m_mapping, dataSize);

    double x(0);
    double y(0);

    for (std::size_t i(0); i < pointsPerSlot; ++i)
    {
        char* pos(m_data.data() + pointSize * i);

        SinglePointTable table(m_schema, pos);
        LinkingPointView view(table);

        x = view.getFieldAs<double>(pdal::Dimension::Id::X, 0);
        y = view.getFieldAs<double>(pdal::Dimension::Id::Y, 0);

        if (Point::exists(x, y))
        {
            m_points[i].atom.store(new Point(x, y));
        }
    }
}

Slot::~Slot()
{
    const std::size_t slotSize(m_points.size() * m_schema.pointSize());
    std::memcpy(m_mapping, m_data.data(), m_data.size());

    if (
            msync(m_mapping, slotSize, MS_SYNC) == -1 ||
            munmap(m_mapping, slotSize) == -1)
    {
        throw std::runtime_error("Couldn't sync mapping");
    }

    for (auto& p : m_points)
    {
        if (p.atom.load()) delete p.atom.load();
    }
}

bool Slot::addPoint(
        PointInfo** toAddPtr,
        const Roller& roller,
        const std::size_t index)
{
    assert(index < m_points.size());

    // TODO Mostly duplicate code with BaseBranch::addPoint.
    bool done(false);
    PointInfo* toAdd(*toAddPtr);
    auto& myPoint(m_points[index].atom);

    if (myPoint.load())
    {
        const Point mid(roller.bbox().mid());

        if (toAdd->point->sqDist(mid) < myPoint.load()->sqDist(mid))
        {
            std::lock_guard<std::mutex> lock(m_locks[index]);
            const Point* curPoint(myPoint.load());

            if (toAdd->point->sqDist(mid) < curPoint->sqDist(mid))
            {
                assert(index * m_schema.pointSize() < m_data.size());

                char* pos(m_data.data() + index * m_schema.pointSize());

                // Pull out the old stored value.
                PointInfo* old(
                        new PointInfo(
                            curPoint,
                            pos,
                            m_schema.pointSize()));

                toAdd->write(pos);
                myPoint.store(toAdd->point);
                delete toAdd;

                *toAddPtr = old;
            }
        }
    }
    else
    {
        std::unique_lock<std::mutex> lock(m_locks[index]);
        if (!myPoint.load())
        {
            assert(index * m_schema.pointSize() < m_data.size());

            char* pos(m_data.data() + index * m_schema.pointSize());
            toAdd->write(pos);
            myPoint.store(toAdd->point);
            delete toAdd;
            done = true;
        }
        else
        {
            lock.unlock();
            done = addPoint(toAddPtr, roller, index);
        }
    }

    return done;
}

bool Slot::hasPoint(const std::size_t index)
{
    return m_points[index].atom.load() != 0;
}

Point Slot::getPoint(const std::size_t index)
{
    Point point;//(INFINITY, INFINITY);

    if (hasPoint(index))
    {
        point = Point(*m_points[index].atom.load());
    }

    return point;
}

std::vector<char> Slot::getPointData(const std::size_t index)
{
    std::vector<char> data;

    if (hasPoint(index))
    {
        char* pos(m_data.data() + index * m_schema.pointSize());
        data.assign(pos, pos + m_schema.pointSize());
    }

    return data;
}

PointMapper::PointMapper(
        const Schema& schema,
        const std::string& filename,
        const std::size_t fileSize,
        const std::size_t firstPoint,
        const std::size_t numPoints)
    : m_schema(schema)
    , m_fd(filename)
    , m_fileSize(fileSize)
    , m_firstPoint(firstPoint)
    , m_slots(numPoints / pointsPerSlot, {0})
    , m_refs(m_slots.size())
    , m_ids(m_slots.size())
    , m_locks(m_slots.size())
{
    if (!fs::fileExists(filename))
    {
        throw std::runtime_error("File does not exist");
    }

    if (
            numPoints % pointsPerSlot != 0 ||
            numPoints * m_schema.pointSize() != fileSize)
    {
        throw std::runtime_error("Invalid arguments to PointMapper");
    }
}

PointMapper::~PointMapper()
{
    for (auto& slot : m_slots)
    {
        if (slot.atom.load())
        {
            delete slot.atom.load();
        }
    }
}

bool PointMapper::addPoint(PointInfo** toAddPtr, const Roller& roller)
{
    bool added(false);

    const std::size_t index(roller.pos());
    assert(index >= m_firstPoint);

    const std::size_t localOffset(index - m_firstPoint);
    const std::size_t slotIndex (localOffset / pointsPerSlot);
    const std::size_t slotOffset(localOffset % pointsPerSlot);

    if (m_slots[slotIndex].atom.load()->addPoint(
            toAddPtr,
            roller,
            slotOffset))
    {
        added = true;

        // Mark this slot as populated.
        const std::size_t globalSlot(m_firstPoint + slotIndex * pointsPerSlot);

        std::lock_guard<std::mutex> lock(m_locks[slotIndex]);
        m_ids[slotIndex].insert(globalSlot);
    }

    return added;
}

bool PointMapper::hasPoint(const std::size_t index)
{
    const std::size_t localOffset(index - m_firstPoint);
    const std::size_t slotIndex (localOffset / pointsPerSlot);
    const std::size_t slotOffset(localOffset % pointsPerSlot);

    return m_slots[slotIndex].atom.load()->hasPoint(slotOffset);
}

Point PointMapper::getPoint(const std::size_t index)
{
    const std::size_t localOffset(index - m_firstPoint);
    const std::size_t slotIndex (localOffset / pointsPerSlot);
    const std::size_t slotOffset(localOffset % pointsPerSlot);

    return m_slots[slotIndex].atom.load()->getPoint(slotOffset);
}

std::vector<char> PointMapper::getPointData(const std::size_t index)
{
    const std::size_t localOffset(index - m_firstPoint);
    const std::size_t slotIndex (localOffset / pointsPerSlot);
    const std::size_t slotOffset(localOffset % pointsPerSlot);

    return m_slots[slotIndex].atom.load()->getPointData(slotOffset);
}

void PointMapper::grow(Clipper* clipper, const std::size_t index)
{
    const std::size_t localOffset(index - m_firstPoint);
    const std::size_t slotIndex (localOffset / pointsPerSlot);
    // const std::size_t slotOffset(localOffset % pointsPerSlot);
    const std::size_t globalSlot(m_firstPoint + slotIndex * pointsPerSlot);

    auto& slot(m_slots[slotIndex].atom);

    std::lock_guard<std::mutex> lock(m_locks[slotIndex]);

    if (!slot.load())
    {
        slot.store(new Slot(m_schema, m_fd, slotIndex * pointsPerSlot));
    }

    if (clipper && clipper->insert(globalSlot))
    {
        m_refs[slotIndex].insert(clipper);
    }
}

void PointMapper::clip(Clipper* clipper, const std::size_t globalSlot)
{
    const std::size_t localOffset(globalSlot - m_firstPoint);
    const std::size_t slotIndex (localOffset / pointsPerSlot);
    const std::size_t slotOffset(localOffset % pointsPerSlot);

    if (slotOffset != 0)
    {
        throw std::runtime_error("Invalid PointMapper state");
    }

    auto& myRef(m_refs[slotIndex]);
    auto& mySlot(m_slots[slotIndex].atom);

    std::lock_guard<std::mutex> lock(m_locks[slotIndex]);
    myRef.erase(clipper);

    if (myRef.empty())
    {
        delete mySlot.load();
        mySlot.store(0);
    }
}

std::vector<std::size_t> PointMapper::ids() const
{
    std::vector<std::size_t> ids;

    for (const auto& slotIdList : m_ids)
    {
        for (const auto slotId : slotIdList)
        {
            ids.push_back(slotId);
        }
    }

    return ids;
}

void PointMapper::finalize(
        S3& output,
        Pool& pool,
        std::vector<std::size_t>& ids,
        const std::size_t start,
        const std::size_t chunkPoints)
{
    if (m_firstPoint < start)
    {
        throw std::runtime_error("Base must end before disk branch depth");
    }

    const std::size_t pointSize(m_schema.pointSize());
    const std::size_t dataSize(chunkPoints * pointSize);

    assert((m_fileSize / pointSize) % chunkPoints == 0);
    assert((m_firstPoint - start) % chunkPoints == 0);

    for (std::size_t pos(0); pos < m_fileSize; pos += dataSize)
    {
        pool.add([this, &output, &ids, chunkPoints, pointSize, dataSize, pos]()
        {
            char* mapping =
                    static_cast<char*>(mmap(
                        0,
                        dataSize,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE,
                        m_fd.id(),
                        pos));

            if (mapping == MAP_FAILED)
            {
                std::cout << "Mapping failed: " << strerror(errno) << std::endl;
                throw std::runtime_error("Could not create mapping!");
            }

            double x(0);
            double y(0);

            bool populated(false);
            std::size_t i(0);

            while (!populated && i < chunkPoints)
            {
                char* point(mapping + pointSize * i++);

                SinglePointTable table(m_schema, point);
                LinkingPointView view(table);

                x = view.getFieldAs<double>(pdal::Dimension::Id::X, 0);
                y = view.getFieldAs<double>(pdal::Dimension::Id::Y, 0);

                if (Point::exists(x, y))
                {
                    populated = true;
                }
            }

            if (populated)
            {
                const std::size_t id(m_firstPoint + pos / pointSize);
                {
                    std::lock_guard<std::mutex> lock(m_locks[0]);
                    ids.push_back(id);
                }

                std::shared_ptr<std::vector<char>> compressed(
                        Compression::compress(
                            mapping,
                            dataSize,
                            m_schema).release());
                output.put(std::to_string(id), *compressed);
            }

            if (
                    msync(mapping, dataSize, MS_SYNC) == -1 ||
                    munmap(mapping, dataSize) == -1)
            {
                throw std::runtime_error("Couldn't sync mapping");
            }
        });
    }
}

} // namespace fs
} // namespace entwine

