#ifndef __GEONET_SPATIAL_DATABASE_H__
#define __GEONET_SPATIAL_DATABASE_H__

#include <memory>
#include <vector>

#include "basic.hpp"



class ISpatialDatabase
{
public:
    virtual double GetDistance(const GpsLocation &one, const GpsLocation &other) const = 0;
    
    virtual bool Store(const NodeLocation &node, bool isNeighbour) = 0;
    virtual std::shared_ptr<NodeLocation> Load(const std::string &nodeId) const = 0;
    virtual bool Update(const NodeLocation &node) const = 0;
    virtual bool Remove(const std::string &nodeId) = 0;
    
    virtual size_t GetColleagueNodeCount() const = 0;
    virtual double GetNeighbourhoodRadiusKm() const = 0;
    
    virtual std::vector<NodeLocation> GetClosestNodes(const GpsLocation &position,
        double Km, size_t maxNodeCount, bool includeNeighbours) const = 0;

    virtual std::vector<NodeLocation> GetRandomNodes(
        uint16_t maxNodeCount, bool includeNeighbours) const = 0;
};




#endif // __GEONET_SPATIAL_DATABASE_H__