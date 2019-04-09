/******************************************************************************
* Copyright (c) 2018, Connor Manning (connor@hobu.co)
*
* Entwine -- Point cloud indexing
*
* Entwine is available under the terms of the LGPL2 license. See COPYING
* for specific license text and more information.
*
******************************************************************************/

#include <entwine/io/binary.hpp>

#include <algorithm>

#include <pdal/PointRef.hpp>

#include <entwine/types/binary-point-table.hpp>
#include <entwine/types/scale-offset.hpp>
#include <entwine/util/executor.hpp>

namespace entwine
{

void Binary::write(
        const arbiter::Endpoint& out,
        const arbiter::Endpoint& tmp,
        const std::string& filename,
        const Bounds& bounds,
        BlockPointTable& src) const
{
    const auto packed(pack(src));
    ensurePut(out, filename + ".bin", packed);
}

void Binary::read(
        const arbiter::Endpoint& out,
        const arbiter::Endpoint& tmp,
        const std::string& filename,
        VectorPointTable& dst) const
{
    auto packed(*ensureGet(out, filename + ".bin"));
    unpack(dst, std::move(packed));
}

std::vector<char> Binary::pack(BlockPointTable& src) const
{
    const uint64_t np(src.size());

    const Schema& outSchema(m_metadata.outSchema());
    VectorPointTable dst(outSchema, np);

    // Handle XYZ separately since we might need to scale/offset them.
    pdal::DimTypeList dimTypes(outSchema.pdalLayout().dimTypes());
    dimTypes.erase(std::remove_if(
                dimTypes.begin(),
                dimTypes.end(),
                [](const pdal::DimType& d)
                {
                    return d.m_id == DimId::X || d.m_id == DimId::Y ||
                        d.m_id == DimId::Z;
                }));

    pdal::PointRef srcPr(src, 0);
    pdal::PointRef dstPr(dst, 0);

    Point p;

    std::unique_ptr<ScaleOffset> so(outSchema.scaleOffset());

    double gps(0);
    std::unique_ptr<SingleScaleOffset> gpsSo(outSchema.gpsScaleOffset());

    for (uint64_t i(0); i < np; ++i)
    {
        srcPr.setPointId(i);
        dstPr.setPointId(i);
        char* pos(dst.getPoint(i));

        // Handle XYZ, applying transformation if needed.
        p.x = srcPr.getFieldAs<double>(DimId::X);
        p.y = srcPr.getFieldAs<double>(DimId::Y);
        p.z = srcPr.getFieldAs<double>(DimId::Z);

        if (so) p = Point::scale(p, so->scale(), so->offset()).round();

        dstPr.setField(DimId::X, p.x);
        dstPr.setField(DimId::Y, p.y);
        dstPr.setField(DimId::Z, p.z);

        // Handle the rest of the dimensions.
        for (const pdal::DimType& dim : dimTypes)
        {
            srcPr.getField(
                    pos + outSchema.pdalLayout().dimOffset(dim.m_id),
                    dim.m_id,
                    dim.m_type);
        }

        if (gpsSo)
        {
            gps = srcPr.getFieldAs<double>(DimId::GpsTime);
            dstPr.setField(
                    DimId::GpsTime,
                    Point::scale(gps, gpsSo->scale(), gpsSo->offset()));
        }
    }

    return dst.data();
}

void Binary::unpack(VectorPointTable& dst, std::vector<char>&& packed) const
{
    VectorPointTable src(m_metadata.outSchema(), std::move(packed));

    const uint64_t np(src.capacity());
    assert(np == dst.capacity());

    // For reading, our destination schema will always be normalized (i.e. XYZ
    // as doubles).  So we can just copy the full dimension list and then
    // transform XYZ in place, if necessary.
    const auto& layout(m_metadata.schema().pdalLayout());
    pdal::DimTypeList dimTypes(layout.dimTypes());

    pdal::PointRef srcPr(src, 0);
    pdal::PointRef dstPr(dst, 0);

    Point p;

    const Schema& outSchema(m_metadata.outSchema());
    std::unique_ptr<ScaleOffset> so(outSchema.scaleOffset());

    double gps(0);
    std::unique_ptr<SingleScaleOffset> gpsSo(outSchema.gpsScaleOffset());

    for (uint64_t i(0); i < np; ++i)
    {
        srcPr.setPointId(i);
        dstPr.setPointId(i);
        char* pos(dst.getPoint(i));

        for (const pdal::DimType& dim : dimTypes)
        {
            srcPr.getField(
                    pos + layout.dimOffset(dim.m_id),
                    dim.m_id,
                    dim.m_type);
        }

        if (so)
        {
            p.x = dstPr.getFieldAs<double>(DimId::X);
            p.y = dstPr.getFieldAs<double>(DimId::Y);
            p.z = dstPr.getFieldAs<double>(DimId::Z);

            p = Point::unscale(p, so->scale(), so->offset());

            dstPr.setField(DimId::X, p.x);
            dstPr.setField(DimId::Y, p.y);
            dstPr.setField(DimId::Z, p.z);
        }

        if (gpsSo)
        {
            gps = srcPr.getFieldAs<double>(DimId::GpsTime);
            dstPr.setField(
                    DimId::GpsTime,
                    Point::unscale(gps, gpsSo->scale(), gpsSo->offset()));
        }
    }

    dst.clear(np);
}

} // namespace entwine

