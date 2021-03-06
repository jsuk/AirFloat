//
//  WebHeaders.h
//  AirFloatCF
//
//  Created by Kristian Trenskow on 5/10/11.
//  Copyright 2011 The Famous Software Company. All rights reserved.
//

#ifndef __WEBHEADERS_H
#define __WEBHEADERS_H

#include <stdint.h>

class WebRequest;
class WebResponse;
class WebConnection;

class WebHeaders {
    
    friend class WebRequest;
    friend class WebResponse;
    friend class WebConnection;
  
public:
    ~WebHeaders();
    
    const char* valueForName(const char* name);
    void addValue(const char* name, const char* format, ...);
    
    uint32_t getCount();
    const char* getNameAtIndex(uint32_t index);
    
    uint32_t getTotalLength();
    uint32_t getContent(void* buffer, uint32_t size);
    
private:
    
    struct header {
        char* name;
        char* value;
    };
    
    WebHeaders();
    uint32_t _parseHeaders(char* buffer, uint32_t length);
    void _setValue(header* header, const char* name, const char* value);

    header* _headers;
    uint32_t _headerCount;
    
};

#endif