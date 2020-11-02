#ifndef COVERAGE_OSIOBSERVER_H
#define COVERAGE_OSIOBSERVER_H

#include <string>

#include "osi/osi_types.h"

namespace coverage
{

/**
 * An interface for monitoring OS state.
 */
class OsiObserver
{
public:
    virtual ~OsiObserver() = default;
    virtual void task_changed(const std::string& process_name,
                              target_pid_t pid,
                              target_pid_t tid) = 0;
};

}

#endif
