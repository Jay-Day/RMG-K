#ifndef RAPHNET_WARNING_POLICY_HPP
#define RAPHNET_WARNING_POLICY_HPP

struct RaphnetWarningPolicy
{
    bool warningShown = false;

    bool observe(int health, bool canShow)
    {
        // Defer the first popup until it is safe to show. Recovery cancels a
        // pending popup; after one has appeared, all later warnings stay inline.
        if (health != 2 || warningShown || !canShow) return false;
        warningShown = true;
        return true;
    }
};

#endif
