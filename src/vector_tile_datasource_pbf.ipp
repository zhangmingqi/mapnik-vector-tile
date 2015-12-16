// mapnik-vector-tile
#include "vector_tile_featureset_pbf.hpp"
#include "vector_tile_projection.hpp"

// mapnik
#include <mapnik/box2d.hpp>
#include <mapnik/coord.hpp>
#include <mapnik/datasource.hpp>
#include <mapnik/feature.hpp>
#include <mapnik/feature_layer_desc.hpp>
#include <mapnik/geom_util.hpp>
#include <mapnik/query.hpp>
#include <mapnik/version.hpp>
#include <mapnik/value_types.hpp>
#include <mapnik/well_known_srs.hpp>

// protozero
#include <protozero/pbf_reader.hpp>

// boost
#include <boost/optional.hpp>

// std
#include <memory>
#include <stdexcept>
#include <string>

namespace mapnik 
{ 

namespace vector_tile_impl 
{

// tile_datasource impl
tile_datasource_pbf::tile_datasource_pbf(protozero::pbf_reader const& layer,
                                         unsigned x,
                                         unsigned y,
                                         unsigned z,
                                         unsigned tile_size)
    : datasource(parameters()),
      desc_("in-memory PBF encoded datasource","utf-8"),
      attributes_added_(false),
      layer_(layer),
      x_(x),
      y_(y),
      z_(z),
      tile_size_(tile_size),
      extent_initialized_(false),
      tile_x_(0.0),
      tile_y_(0.0),
      scale_(0.0),
      layer_extent_(0),
      version_(1) // Version == 1 is the default because it was not required until v2 to have this field
{
    double resolution = mapnik::EARTH_CIRCUMFERENCE/(1 << z_);
    tile_x_ = -0.5 * mapnik::EARTH_CIRCUMFERENCE + x_ * resolution;
    tile_y_ =  0.5 * mapnik::EARTH_CIRCUMFERENCE - y_ * resolution;

    protozero::pbf_reader val_msg;

    // Check that the required fields exist for version 2 of the MVT specification
    bool has_name = false;
    bool has_extent = false;

    while (layer_.next())
    {
        switch(layer_.tag())
        {
            case 1:
                name_ = layer_.get_string();
                has_name = true;
                break;
            case 2:
                features_.push_back(layer_.get_message());
                break;
            case 3:
                layer_keys_.push_back(layer_.get_string());
                break;
            case 4:
                val_msg = layer_.get_message();
                while (val_msg.next())
                {
                    switch(val_msg.tag()) {
                        case 1:
                            layer_values_.push_back(val_msg.get_string());
                            break;
                        case 2:
                            layer_values_.push_back(val_msg.get_float());
                            break;
                        case 3:
                            layer_values_.push_back(val_msg.get_double());
                            break;
                        case 4:
                            layer_values_.push_back(val_msg.get_int64());
                            break;
                        case 5:
                            layer_values_.push_back(val_msg.get_uint64());
                            break;
                        case 6:
                            layer_values_.push_back(val_msg.get_sint64());
                            break;
                        case 7:
                            layer_values_.push_back(val_msg.get_bool());
                            break;
                        default:
                            throw std::runtime_error("unknown Value type " + std::to_string(layer_.tag()) + " in layer.values");
                    }
                }
                break;
            case 5:
                layer_extent_ = layer_.get_uint32();
                has_extent = true;
                break;
            case 15:
                version_ = layer_.get_uint32();
                break;
            default:
                throw std::runtime_error("unknown field type " + std::to_string(layer_.tag()) + " in layer");
        }
    }
    if (version_ == 2)
    {
        // Check that all required
        if (!has_name)
        {
            throw std::runtime_error("The required name field is missing in a vector tile layer. Tile does not comply with Version 2 of the Mapbox Vector Tile Specification.");
        }
        else if (!has_extent)
        {
            throw std::runtime_error("The required extent field is missing in the layer " + name_ + ". Tile does not comply with Version 2 of the Mapbox Vector Tile Specification.");
        }
        scale_ = (static_cast<double>(layer_extent_) / tile_size_) * tile_size_/resolution;
    }
    else if (version_ == 1)
    {
        if (!has_name)
        {
            throw std::runtime_error("The required name field is missing in a vector tile layer.");
        }
        if (!has_extent)
        {
            // the extent was never set in a version 1 tile
            // so lets try to process with the default extent
            // this was not declared as required in version 1 of the
            // the specification
            layer_extent_ = 4096; 
        }
        scale_ = (static_cast<double>(layer_extent_) / tile_size_) * tile_size_/resolution;
    }
}

tile_datasource_pbf::~tile_datasource_pbf() {}

datasource::datasource_t tile_datasource_pbf::type() const
{
    return datasource::Vector;
}

featureset_ptr tile_datasource_pbf::features(query const& q) const
{
    if (version_  > 2 || version_ < 1 || features_.empty())
    {
        // From spec:
        // When a Vector Tile consumer encounters a Vector Tile layer with an unknown version, 
        // it MAY make a best-effort attempt to interpret the layer, or it MAY skip the layer. 
        // In either case it SHOULD continue to process subsequent layers in the Vector Tile.
    
        // Therefore if version is invalid we will just return pointer
        return featureset_ptr();
    }
    mapnik::filter_in_box filter(q.get_bbox());
    return std::make_shared<tile_featureset_pbf<mapnik::filter_in_box> >
        (filter, get_tile_extent(), q.get_unbuffered_bbox(), q.property_names(), features_, tile_x_, tile_y_, scale_, layer_keys_, layer_values_, version_);
}

featureset_ptr tile_datasource_pbf::features_at_point(coord2d const& pt, double tol) const
{
    if (version_  > 2 || version_ < 1)
    {
        // From spec:
        // When a Vector Tile consumer encounters a Vector Tile layer with an unknown version, 
        // it MAY make a best-effort attempt to interpret the layer, or it MAY skip the layer. 
        // In either case it SHOULD continue to process subsequent layers in the Vector Tile.
    
        // Therefore if version is invalid we will just return pointer
        return featureset_ptr();
    }
    mapnik::filter_at_point filter(pt,tol);
    std::set<std::string> names;
    for (auto const& key : layer_keys_)
    {
        names.insert(key);
    }
    return std::make_shared<tile_featureset_pbf<filter_at_point> >
        (filter, get_tile_extent(), get_tile_extent(), names, features_, tile_x_, tile_y_, scale_, layer_keys_, layer_values_, version_);
}

void tile_datasource_pbf::set_envelope(box2d<double> const& bbox)
{
    extent_initialized_ = true;
    extent_ = bbox;
}

box2d<double> tile_datasource_pbf::get_tile_extent() const
{
    spherical_mercator merc(tile_size_);
    double minx,miny,maxx,maxy;
    merc.xyz(x_,y_,z_,minx,miny,maxx,maxy);
    return box2d<double>(minx,miny,maxx,maxy);
}

box2d<double> tile_datasource_pbf::envelope() const
{
    if (!extent_initialized_)
    {
        extent_ = get_tile_extent();
        extent_initialized_ = true;
    }
    return extent_;
}

boost::optional<mapnik::datasource_geometry_t> tile_datasource_pbf::get_geometry_type() const
{
    return mapnik::datasource_geometry_t::Collection;
}

layer_descriptor tile_datasource_pbf::get_descriptor() const
{
    if (!attributes_added_)
    {
        for (auto const& key : layer_keys_)
        {
            // Object type here because we don't know the precise value until features are unpacked
            desc_.add_descriptor(attribute_descriptor(key, Object));
        }
        attributes_added_ = true;
    }
    return desc_;
}

} // end ns vector_tile_impl

} // end ns mapnik
