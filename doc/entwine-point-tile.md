# Entwine Point Tile

Entwine Point Tile (EPT) is a simple and flexible octree-based storage format
for point cloud data. It provides a number of simple choices that make it
suitable for data storage and organization for statically-accessed point cloud
data. EPT grew from Hobu, Inc.'s experience developing a server-based point
cloud web service called [https://greyhound.io](Greyhound), collaboration with
the [http://potree.org](Potree) and
[https://github.com/potree/PotreeConverter](PotreeConverter) projects, and
development of [http://laszip.org](LASzip) technology for JavaScript browsers.


This is a working draft of an Entwine Point Tiles specification, which is is
complimented by an open source implementation at https://entwine.io which works
along with browser-based open source visualization clients Potree and Plasio.

## Background

Visualization of large-scale point clouds is a primary desire of those looking
to achieve a point cloud web service infrastructure.  The visualization problem
is demanding in a number of ways. First, responses must be quick, which means
optimizing both the number of requests and their size to a remote data source.
Second, the data must be organized in such a way that a client can control
their own access throughout the data domain. Third, the content encoding must
be openly configurable and flexible enough to encode any possible point cloud
type. Finally, the protocol a client must use to request data must be simple
enough for developers to leverage without significant software development.


When combined with cloud computing, the application of the tiling approach to
raster data allowed numerous client applications to leverage extremely large
datasources while controlling the pace and content of requested data. A similar
need exists in point cloud data, but point cloud tiles add some extra
complications in contrast to raster data. Of course, the extra dimensionality
is a significant part of the challenge, but so is the sometimes irregular
nature of large point cloud collections. Organizations that optimize for
density are stymied by collections of large density variations (such as long
strips of roadway LiDAR captures over a city or a state). Optimizations that
optimize for consistent spacing must account for variable density, which
challenges prediction of storage and access.

### Idempotent requests

Unlike a raster pyramid, a point cloud tileset built in an additive approach
duplicates more than a single band of conveniently compressible content.
Each pyramid level adds significant data overhead to fetch, store, compress,
and filter. For a point cloud tiling scheme to scale to point clouds representing
all of the aerial LiDAR for a country, for example, a duplicative tree
might add a full 60% of request and storage overhead – overhead that is felt
throughout in performance and cost.

Like existing raster or vector tiling approaches, each request to an Entwine
Point Tile set is idempotent. This indemnity allows servers to treat data
atomically, and it allows clients to parallelize simply.

### Client request control

Entwine Point Tiles borrows the hierarchy metadata approach that
PotreeConverter implemented to free itself from having to choose between a
space-organized or a density-organized approach. The hierarchy allows clients
"peek" down the octree from a node position to provide an exact count of
upcoming points that would be selected. This information allows clients to
adjust their pacing while batching their round trips appropriately. Without
this mechanism, clients are at the mercy of their responses, and they have
little control over how long they must wait to complete a response cycle.



## Metadata files

### ept.json

`ept.json` is core metadata required to interpret the contents of an EPT
dataset. All EPT data resources must have an ``ept.json`` file at their root
level. An example file might look like the following:


```json
{
    "bounds": [634962.0, 848881.0, -1818.0, 639620.0, 853539.0, 2840.0],
    "boundsConforming": [635577.0, 848882.0, 406.0, 639004.0, 853538.0, 616.0],
    "dataType": "laszip",
    "hierarchyStep": 6,
    "hierarchyType": "json",
    "numPoints": 10653336,
    "schema": [
        { "name": "X", "type": "int32", "scale": 0.01, "offset": 637291.0 },
        { "name": "Y", "type": "int32", "scale": 0.01, "offset": 851210.0 },
        { "name": "Z", "type": "int32", "scale": 0.01, "offset": 511.0 },
        { "name": "Intensity", "type": "uint16" },
        { "name": "ReturnNumber", "type": "uint8" },
        { "name": "NumberOfReturns", "type": "uint8" },
        { "name": "ScanDirectionFlag", "type": "uint8" },
        { "name": "EdgeOfFlightLine", "type": "uint8" },
        { "name": "Classification", "type": "uint8" },
        { "name": "ScanAngleRank", "type": "float" },
        { "name": "UserData", "type": "uint8" },
        { "name": "PointSourceId", "type": "uint16" },
        { "name": "GpsTime", "type": "double" },
        { "name": "Red", "type": "uint16" },
        { "name": "Green", "type": "uint16" },
        { "name": "Blue", "type": "uint16" }
    ],
    "srs" : {
        "authority": "EPSG",
        "horizontal": "3857",
        "vertical": "5703",
        "wkt":
    },
    "ticks" : 256,
    "version" : "2.0.0"
}
```

#### bounds
A 6-tuple in the form `[xmin, ymin, zmin, xmax, ymax, zmax]` describing the
**cubic** bounds of the octree indexing structure.

NOTE: This value is always in native coordinate space, any `scale` or `offset`
values will not have been applied, and it presented in the coordinate system
matching the `srs` value.

#### boundsConforming

A 6-tuple in the form `[xmin, ymin, zmin, xmax, ymax, zmax]` describing the
narrowest or tightest bounds conforming to the maximal extents of the data.

NOTE: This value is always in native coordinate space, any `scale` or `offset`
values will not have been applied, and it presented in the coordinate system
matching the `srs` value.



#### dataType
A string describing the binary format of the tiled point cloud data.  Possible values:

- `laszip`: Point cloud files are [LASzip](https://laszip.org/) compressed, with file extension `.laz`.
- `binary`: Point cloud files are stored as uncompressed binary data in the format matching the `schema`, with file extension `.bin`.

#### hierarchyStep
This value indicates the octree depth modulo at which the hierarchy storage is split up.  This value may not be present at all, which indicates that the hierarchy is stored contiguously without any splitting.  See the `Hierarchy` section.

#### hierarchyType
A string describing the hierarchy storage format.  See the `Hierarchy` section.  The hierarchy itself is always represented as JSON, but this value may indicate a compression method for this JSON.  Possible values:

- `json`: Hierarchy is stored uncompressed with file extension `.json`.
- `gzip`: Hierarchy is stored as [Gzip](https://www.gnu.org/software/gzip/) compressed JSON with file extension `.json.gz`.
- `encoded`: Servers use a registered content encoding such as `Content-Encoding: gzip`


#### numPoints
A number indicating the total, exact, and inclusive number of points indexed
into this EPT dataset.

#### offset
The offset at which this data was indexed - an array of length 3.  This value
will not exist for absolutely positioned data.  If `offset` is present, then
absolutely positioned values for spatial coordinates can be determined as
`absolutelyPositionedValue = serializedValue * scale + offset`.  Note that for
a `dataType` of `laszip`, offset information must be read from the LAZ header
since individual files may be serialized with a local offset.

#### scale
The scale factor for this data.  May be either a number, an array of length 3,
or may not be present at all in the case of absolutely positioned data.  If
`scale` is present, then absolutely positioned values for spatial coordinates
can be determined as `absolutelyPositionedValue = serializedValue * scale +
offset`.

#### schema
An array of objects that represent the indexed dimensions - every dimension has
a `name` and a `type`.  Spatial coordinates with names `X`, `Y`, and `Z` will
always be present as the first 3 values of this array.  Valid values for `type`
are `uint8`, `uint16`, `uint32`, `uint64`, `int8`, `int16`, `int32`, `int64`,
`float`, and `double`.

For a `dataType` of `binary`, the `schema` provides information on the binary
contents of each file.  However for `laszip` data, the file should be parsed
according to its header, as individual [LASzip
formats](https://www.asprs.org/wp-content/uploads/2010/12/LAS_1_4_r13.pdf) may
combine dimension values.  For example, for point format IDs 0-4,
`ReturnNumber`, `NumberOfReturns`, `ScanDirectionFlag`, and `EdgeOfFlightLine`
dimensions are bit-packed into a single byte.

#### srs

An object describing the spatial reference system for this indexed dataset, or
may not exist if a spatial reference could not be determined and was not set
manually.  In this object there are string keys with string values of the
following descriptions:

    - `authority`: Typically `"EPSG"` (if present), this value represents the authority for the horizontal and vertical codes.
    - `horizontal`: Horizontal coordinate system code with respect to the given `authority`.  If present, `authority` must exist.
    - `vertical`: Vertical coordinate system code with respect to the given `authority`.  If present, `authority` must exist.
    - `wkt`: A WKT specification of the spatial reference.

#### ticks
This value represents the number of ticks in one dimension for the grid size
used for the octree.  For example, a `ticks` value of `256` means that the root
volume of the octree is the `bounds` cube at a resolution of `256 * 256 * 256`.

For aerial LiDAR data which tends to be mostly flat in the Z-range, for
example, this would loosely correspond to a practical resolution of `256 * 256`
points per volume.  Each subsequent depth bisects this volume, leading to 8
sub-volumes with the same grid size, which corresponds to double the resolution
in each dimension.

#### version
Version string.

### ept-files.json
This file is a JSON array with an object entry for each point cloud source file
indexed as input.  The length of this array specifies the total number of input
files.  Each entry contains sparse metrics about each input file and its status
in the EPT build process.  An example might look like:

```json
[
    {
        "path" : "non-point-cloud-file.txt",
        "status" : "omitted"
    },
    {
        "bounds": [635577.79, 848882.15, 406.14, 639003.73, 853537.66, 615.26],
        "numPoints": 10653336,
        "path" : "https://entwine.io/data/autzen.laz",
        "pointStats" : { "inserts" : 10653336, "outOfBounds" : 0 },
        "srs" : "wkt-here...",
        "status" : "inserted"
    }
]
```

### ept-build.json
This file may be ignored by EPT reader clients, and is used to store
builder-specific information which may be necessary to specify parameters
required to continue an existing build at a later time.

## Point cloud data
The point data itself is arranged in a 3D analogous manner to [slippy
map](https://wiki.openstreetmap.org/wiki/Slippy_map_tilenames) tiling schemes.
The filename scheme `Zoom-X-Y` is expanded to three dimensions as
`Depth-X-Y-Z`.  As opposed to raster tiling schemes where coarser-resolution
data is replaced as its sub-tiles are traversed, the point cloud data is
instead additive, meaning that full-resolution is obtained by the accumulated
traversal from depth `0` to the last depth containing a positive point count
for the selected area.

The root volume, is always named `0-0-0-0`.  This volume always represents the
volume of the cubic `bounds` value from `entwine.json` split in each dimension
by `ticks`.  So a `bounds` with a volume of 256 cubic meters, with a `ticks`
value of `256`, corresponds to a root node with a maximum of 1 point per cubic
meter.

Each node at depth `D` may have up to 8 child nodes at depth `D + 1` which
represent bisected sub-volumes.  To zoom in, depth `D` is incremented by one,
and each of `X`, `Y`, and `Z` are doubled and then possibly incremented by one.
Coordinates are treated as Cartesian during this bisection, so `X -> 2 * X`
represents the sub-volume where the new maximum `X` value is the midpoint from
its parent, and `X -> 2 * X + 1` represents the sub-volume where the new
minimum `X` value is the midpoint from its parent.

In web-mercator, for example, where `X` increases going eastward and `Y`
increases going northward, a traversal from `0-0-0-0` to `1-1-0-1` represents a
traversal to the east/south/down sub-volume.

There is no fixed maximum resolution depth, instead the tiles must be traversed
until no more data exists.  For look-ahead capability, see `Hierarchy`.

## Hierarchy
The hierarchy section contains information about what nodes exist and how many
points they contain.  The file format is simple JSON object, with string keys
of `D-X-Y-Z` mapping to a point count for the corresponding file.  The root
file of the hierarchy data exists at `ept-hierarchy/0-0-0-0.json`.  For
example:

```json
{
    "0-0-0-0": 65341,
        "1-0-0-0": 438,
            "2-0-1-0": 322,
        "1-0-0-1": 56209,
            "2-0-1-2": 4332,
            "2-1-1-2": 20300,
            "2-1-1-3": 64020,
                "3-2-3-6": 32004,
                    "4-4-6-12": 1500,
                    "4-5-6-13": 2400,
                "3-3-3-7": 542,
        "1-0-1-0": 30390,
            "2-1-2-0": 2300,
        "1-1-1-1": 2303
}
```

Note that this sample is visually arranged hierarchically for clarity, which is
not the case in practice.

Furthermore, if `entwine.json` contains a `hierarchyStep` key, then the
hierarchy is split into multiple files whenever `D % hierarchyStep == 0`.  The
next filename corresponds to the `D-X-Y-Z` value of the local root node at this
depth.  The above example, with a `hierarchyStep` of `3`, would look like:

`ept-hierarchy/0-0-0-0.json`
```json
{
    "0-0-0-0": 65341,
        "1-0-0-0": 438,
            "2-0-1-0": 322,
        "1-0-0-1": 56209,
            "2-0-1-2": 4332,
            "2-1-1-2": 20300,
            "2-1-1-3": 64020,
                "3-2-3-6": 32004,
                "3-3-3-7": 542,
        "1-0-1-0": 30390,
            "2-1-2-0": 2300,
        "1-1-1-1": 2303
}
```

`ept-hierarchy/3-2-3-6.json`
```json
{
    "3-2-3-6": 32004,
        "4-4-6-12": 1500,
        "4-5-6-13": 2400
}
```

`ept-hierarchy/3-3-3-7.json`
```json
{
    "3-3-3-7": 542
}
```

The local root node of each subfile is duplicated in its parent file so that
child hierarchy files can be guaranteed to exist during traversal if their key
exists in the parent file.

## Source file metadata
To be lossless and facilitate a full reconstitution of original source data
files from an EPT index, full metadata for each input file should be retained.
The contents of `entwine-files.json` give the total number `N` of input files,
and full file metadata can be found in the structure:

```
ept-metadata/0.json
ept-metadata/1.json
...
ept-metadata/<N-1>.json
```

