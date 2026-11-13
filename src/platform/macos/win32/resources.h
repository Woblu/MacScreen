#pragma once

struct MacEmbeddedResource {
    unsigned id;
    const unsigned char* data;
    unsigned size;
};

extern const MacEmbeddedResource g_macEmbeddedResources[];
extern const unsigned g_macEmbeddedResourceCount;
