/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. 
 *
 *    Copyright 2017 (c) Fraunhofer IOSB (Author: Julius Pfrommer)
 *    Copyright 2017 (c) Stefan Profanter, fortiss GmbH
 *    Copyright 2018 (c) Ari Breitkreuz, fortiss GmbH
 *    Copyright 2018 (c) Thomas Stalder, Blue Time Concept SA
 */

#include "ua_subscription.h"
#include "ua_server_internal.h"
#include "ua_types_encoding_binary.h"
#include "ua_subscription_events.h"

#ifdef UA_ENABLE_SUBSCRIPTIONS /* conditional compilation */

#define UA_VALUENCODING_MAXSTACK 512

UA_MonitoredItem *
UA_MonitoredItem_new() {
    /* Allocate the memory */
    UA_MonitoredItem *newItem =
            (UA_MonitoredItem *) UA_calloc(1, sizeof(UA_MonitoredItem));
    if(!newItem)
        return NULL;

    /* Remaining members are covered by calloc zeroing out the memory */
    newItem->timestampsToReturn = UA_TIMESTAMPSTORETURN_SOURCE;
    TAILQ_INIT(&newItem->queue);
    return newItem;
}

#ifdef UA_ENABLE_EVENTS
static UA_StatusCode removeMonitoredItemFromNodeCallback(UA_Server *server, UA_Session *session, UA_Node *node,
                                                         void *data) {
    /* data is the monitoredItemID */
    UA_MonitoredItemQueueEntry *entry, *tmp_entry;
    LIST_FOREACH_SAFE(entry, &((UA_ObjectNode *) node)->monitoredItemQueue, listEntry, tmp_entry) {
        if (entry->mon->monitoredItemId == *(UA_UInt32 *)data) {
            LIST_REMOVE(entry, listEntry);
            UA_free(entry);
        }
    }
    return UA_STATUSCODE_GOOD;
}
#endif

void
MonitoredItem_delete(UA_Server *server, UA_MonitoredItem *monitoredItem) {
    UA_Subscription *sub = monitoredItem->subscription;
    UA_LOG_DEBUG_SESSION(server->config.logger, sub->session,
                        "Subscription %u | MonitoredItem %i | "
                        "Delete the MonitoredItem", sub->subscriptionId,
                        monitoredItem->monitoredItemId);

    /* Remove the sampling callback */
    MonitoredItem_unregisterSampleCallback(server, monitoredItem);

    /* Clear the queued notifications */
    UA_Notification *notification, *notification_tmp;
    TAILQ_FOREACH_SAFE(notification, &monitoredItem->queue, listEntry, notification_tmp) {
        /* Remove the item from the queues */
        TAILQ_REMOVE(&monitoredItem->queue, notification, listEntry);
        TAILQ_REMOVE(&sub->notificationQueue, notification, globalEntry);
        --sub->notificationQueueSize;

        if (monitoredItem->monitoredItemType == UA_MONITOREDITEMTYPE_CHANGENOTIFY) {
            UA_DataValue_deleteMembers(&notification->data.value);
        } else if (monitoredItem->monitoredItemType == UA_MONITOREDITEMTYPE_EVENTNOTIFY) {
            UA_EventFieldList_delete(notification->data.event->fields);
            /* EventFilterResult currently isn't being used
            UA_EventFilterResult_delete(notification->data.event->result); */
            UA_free(notification->data.event);
        }
        UA_free(notification);
        monitoredItem->queueSize = 0;
    }
#ifdef UA_ENABLE_EVENTS
    if (monitoredItem->monitoredItemType == UA_MONITOREDITEMTYPE_EVENTNOTIFY) {
        /* Remove the monitored item from the node queue */
        UA_Server_editNode(server, NULL, &monitoredItem->monitoredNodeId, removeMonitoredItemFromNodeCallback,
                           &monitoredItem->monitoredItemId);
        /* Delete the event filter */
        UA_EventFilter_delete(monitoredItem->filter.eventFilter);
    }
#endif
    /* Remove the monitored item */
    UA_String_deleteMembers(&monitoredItem->indexRange);
    UA_ByteString_deleteMembers(&monitoredItem->lastSampledValue);
    UA_Variant_deleteMembers(&monitoredItem->lastValue);
    UA_NodeId_deleteMembers(&monitoredItem->monitoredNodeId);
    UA_Server_delayedFree(server, monitoredItem);
}

UA_StatusCode MonitoredItem_ensureQueueSpace(UA_Server *server, UA_MonitoredItem *mon) {
    if(mon->queueSize <= mon->maxQueueSize)
        return UA_STATUSCODE_GOOD;

    /* Remove notifications until the queue size is reached */
    UA_Subscription *sub = mon->subscription;
    while(mon->queueSize > mon->maxQueueSize) {
        UA_assert(mon->queueSize >= 2); /* At least two Notifications in the queue */

        /* Make sure that the MonitoredItem does not lose its place in the
         * global queue when notifications are removed. Otherwise the
         * MonitoredItem can "starve" itself by putting new notifications always
         * at the end of the global queue and removing the old ones.
         *
         * - If the oldest notification is removed, put the second oldest
         *   notification right behind it.
         * - If the newest notification is removed, put the new notification
         *   right behind it. */

        UA_Notification *del; /* The notification that will be deleted */
        UA_Notification *after_del; /* The notification to keep and move after del */
        if(mon->discardOldest) {
            /* Remove the oldest */
            del = TAILQ_FIRST(&mon->queue);
            after_del = TAILQ_NEXT(del, listEntry);
        } else {
            /* Remove the second newest (to keep the up-to-date notification) */
            after_del = TAILQ_LAST(&mon->queue, NotificationQueue);
            del = TAILQ_PREV(after_del, NotificationQueue, listEntry);
        }

        /* Move after_del right after del in the global queue */
        TAILQ_REMOVE(&sub->notificationQueue, after_del, globalEntry);
        TAILQ_INSERT_AFTER(&sub->notificationQueue, del, after_del, globalEntry);

        /* Remove the notification from the queues */
        TAILQ_REMOVE(&mon->queue, del, listEntry);
        TAILQ_REMOVE(&sub->notificationQueue, del, globalEntry);
#ifdef UA_ENABLE_EVENTS
        /* TODO: provide additional protection for overflowEvents according to specification */
        /* removing an overflowEvent should not reduce the queueSize */
        UA_NodeId overflowId = UA_NODEID_NUMERIC(0, UA_NS0ID_SIMPLEOVERFLOWEVENTTYPE);
        if (!(del->data.event->fields->eventFieldsSize == 1
              && del->data.event->fields->eventFields->type == &UA_TYPES[UA_TYPES_NODEID]
              && UA_NodeId_equal((UA_NodeId *)del->data.event->fields->eventFields->data, &overflowId))) {
            --mon->queueSize;
            --sub->notificationQueueSize;
        }
#else
        --mon->queueSize;
        --sub->notificationQueueSize;
#endif

        /* Free the notification */
        if(mon->monitoredItemType == UA_MONITOREDITEMTYPE_CHANGENOTIFY) {
            UA_DataValue_deleteMembers(&del->data.value);
        } else if (mon->monitoredItemType == UA_MONITOREDITEMTYPE_EVENTNOTIFY) {
#ifdef UA_ENABLE_EVENTS
            /* EventFilterResult currently isn't being used
            UA_EventFilterResult_delete(del->data.event->result); */
            UA_EventFieldList_delete(del->data.event->fields);
            UA_free(del->data.event);

            /* cause an overflowEvent */
            /* an overflowEvent does not care about event filters and as such will not be "triggered" correctly.
             * Instead, a notification will be inserted into the queue which includes only the nodeId of the
             * overflowEventType. It is up to the client to check for possible overflows.
             */
            UA_Notification *overflowNotification = (UA_Notification *) UA_malloc(sizeof(UA_Notification));
            if (!overflowNotification) {
                return UA_STATUSCODE_BADOUTOFMEMORY;
            }

            overflowNotification->data.event = (UA_EventNotification *) UA_malloc(sizeof(UA_EventNotification));
            if (!overflowNotification->data.event) {
                UA_free(overflowNotification);
                return UA_STATUSCODE_BADOUTOFMEMORY;
            }

            overflowNotification->data.event->fields = UA_EventFieldList_new();
            if (!overflowNotification->data.event->fields) {
                UA_free(overflowNotification->data.event);
                UA_free(overflowNotification);
                return UA_STATUSCODE_BADOUTOFMEMORY;
            }
            UA_EventFieldList_init(overflowNotification->data.event->fields);

            overflowNotification->data.event->fields->eventFields = UA_Variant_new();
            if (!overflowNotification->data.event->fields->eventFields) {
                UA_free(overflowNotification->data.event->fields);
                UA_free(overflowNotification->data.event);
                UA_free(overflowNotification);
                return UA_STATUSCODE_BADOUTOFMEMORY;
            }
            UA_Variant_init(overflowNotification->data.event->fields->eventFields);

            overflowNotification->data.event->fields->eventFieldsSize = 1;
            UA_Variant_setScalarCopy(overflowNotification->data.event->fields->eventFields,
                                              &overflowId, &UA_TYPES[UA_TYPES_NODEID]);
            overflowNotification->mon = mon;
            if (mon->discardOldest) {
                TAILQ_INSERT_HEAD(&mon->queue, overflowNotification, listEntry);
                TAILQ_INSERT_HEAD(&mon->subscription->notificationQueue, overflowNotification, globalEntry);
            } else {
                TAILQ_INSERT_TAIL(&mon->queue, overflowNotification, listEntry);
                TAILQ_INSERT_TAIL(&mon->subscription->notificationQueue, overflowNotification, globalEntry);
            }
#endif
        }

        /* Work around a false positive in clang analyzer */
#ifndef __clang_analyzer__
        UA_free(del);
#endif
    }

    if(mon->monitoredItemType == UA_MONITOREDITEMTYPE_CHANGENOTIFY) {
        /* Get the element that carries the infobits */
        UA_Notification *notification = NULL;
        if(mon->discardOldest)
            notification = TAILQ_FIRST(&mon->queue);
        else
            notification = TAILQ_LAST(&mon->queue, NotificationQueue);
        UA_assert(notification);

        if(mon->maxQueueSize > 1) {
            /* Add the infobits either to the newest or the new last entry */
            notification->data.value.hasStatus = true;
            notification->data.value.status |= (UA_STATUSCODE_INFOTYPE_DATAVALUE |
                                                UA_STATUSCODE_INFOBITS_OVERFLOW);
        } else {
            /* If the queue size is reduced to one, remove the infobits */
            notification->data.value.status &= ~(UA_StatusCode)(UA_STATUSCODE_INFOTYPE_DATAVALUE |
                                                                UA_STATUSCODE_INFOBITS_OVERFLOW);
        }
    }

    /* TODO: Infobits for Events? */
    return UA_STATUSCODE_GOOD;
}

#define ABS_SUBTRACT_TYPE_INDEPENDENT(a,b) ((a)>(b)?(a)-(b):(b)-(a))

static UA_INLINE UA_Boolean
outOfDeadBand(const void *data1, const void *data2, const size_t index, const UA_DataType *type, const UA_Double deadbandValue) {
    if (type == &UA_TYPES[UA_TYPES_SBYTE]) {
        if (ABS_SUBTRACT_TYPE_INDEPENDENT(((const UA_SByte*)data1)[index], ((const UA_SByte*)data2)[index]) <= deadbandValue)
            return false;
    } else
    if (type == &UA_TYPES[UA_TYPES_BYTE]) {
        if (ABS_SUBTRACT_TYPE_INDEPENDENT(((const UA_Byte*)data1)[index], ((const UA_Byte*)data2)[index]) <= deadbandValue)
                return false;
    } else
    if (type == &UA_TYPES[UA_TYPES_INT16]) {
        if (ABS_SUBTRACT_TYPE_INDEPENDENT(((const UA_Int16*)data1)[index], ((const UA_Int16*)data2)[index]) <= deadbandValue)
            return false;
    } else
    if (type == &UA_TYPES[UA_TYPES_UINT16]) {
        if (ABS_SUBTRACT_TYPE_INDEPENDENT(((const UA_UInt16*)data1)[index], ((const UA_UInt16*)data2)[index]) <= deadbandValue)
            return false;
    } else
    if (type == &UA_TYPES[UA_TYPES_INT32]) {
        if (ABS_SUBTRACT_TYPE_INDEPENDENT(((const UA_Int32*)data1)[index], ((const UA_Int32*)data2)[index]) <= deadbandValue)
            return false;
    } else
    if (type == &UA_TYPES[UA_TYPES_UINT32]) {
        if (ABS_SUBTRACT_TYPE_INDEPENDENT(((const UA_UInt32*)data1)[index], ((const UA_UInt32*)data2)[index]) <= deadbandValue)
            return false;
    } else
    if (type == &UA_TYPES[UA_TYPES_INT64]) {
        if (ABS_SUBTRACT_TYPE_INDEPENDENT(((const UA_Int64*)data1)[index], ((const UA_Int64*)data2)[index]) <= deadbandValue)
            return false;
    } else
    if (type == &UA_TYPES[UA_TYPES_UINT64]) {
        if (ABS_SUBTRACT_TYPE_INDEPENDENT(((const UA_UInt64*)data1)[index], ((const UA_UInt64*)data2)[index]) <= deadbandValue)
            return false;
    } else
    if (type == &UA_TYPES[UA_TYPES_FLOAT]) {
        if (ABS_SUBTRACT_TYPE_INDEPENDENT(((const UA_Float*)data1)[index], ((const UA_Float*)data2)[index]) <= deadbandValue)
            return false;
    } else
    if (type == &UA_TYPES[UA_TYPES_DOUBLE]) {
        if (ABS_SUBTRACT_TYPE_INDEPENDENT(((const UA_Double*)data1)[index], ((const UA_Double*)data2)[index]) <= deadbandValue)
            return false;
    }
    return true;
}

static UA_INLINE UA_Boolean
updateNeededForFilteredValue(const UA_Variant *value, const UA_Variant *oldValue, const UA_Double deadbandValue) {
    if (value->arrayLength != oldValue->arrayLength) {
        return true;
    }
    if (value->type != oldValue->type) {
        return true;
    }
    if (UA_Variant_isScalar(value)) {
        return outOfDeadBand(value->data, oldValue->data, 0, value->type, deadbandValue);
    } else {
        for (size_t i = 0; i < value->arrayLength; ++i) {
            if (outOfDeadBand(value->data, oldValue->data, i, value->type, deadbandValue))
                return true;
        }
    }
    return false;
}

/* Errors are returned as no change detected */
static UA_Boolean
detectValueChangeWithFilter(UA_MonitoredItem *mon, UA_DataValue *value,
                            UA_ByteString *encoding) {
    if (isDataTypeNumeric(value->value.type)
            && (mon->filter.trigger == UA_DATACHANGETRIGGER_STATUSVALUE
                || mon->filter.trigger == UA_DATACHANGETRIGGER_STATUSVALUETIMESTAMP)) {
        if (mon->filter.deadbandType == UA_DEADBANDTYPE_ABSOLUTE) {
            if (!updateNeededForFilteredValue(&value->value, &mon->lastValue, mon->filter.deadbandValue))
                return false;
        } /*else if (mon->filter.deadbandType == UA_DEADBANDTYPE_PERCENT) {
            // TODO where do this EURange come from ?
            UA_Double deadbandValue = fabs(mon->filter.deadbandValue * (EURange.high-EURange.low));
            if (!updateNeededForFilteredValue(value->value, mon->lastValue, deadbandValue))
                return false;
        }*/
    }

    /* Encode the data for comparison */
    size_t binsize = UA_calcSizeBinary(value, &UA_TYPES[UA_TYPES_DATAVALUE]);
    if(binsize == 0)
        return false;

    /* Allocate buffer on the heap if necessary */
    if(binsize > UA_VALUENCODING_MAXSTACK &&
       UA_ByteString_allocBuffer(encoding, binsize) != UA_STATUSCODE_GOOD)
        return false;

    /* Encode the value */
    UA_Byte *bufPos = encoding->data;
    const UA_Byte *bufEnd = &encoding->data[encoding->length];
    UA_StatusCode retval = UA_encodeBinary(value, &UA_TYPES[UA_TYPES_DATAVALUE],
                                           &bufPos, &bufEnd, NULL, NULL);
    if(retval != UA_STATUSCODE_GOOD)
        return false;

    /* The value has changed */
    encoding->length = (uintptr_t)bufPos - (uintptr_t)encoding->data;
    return !mon->lastSampledValue.data || !UA_String_equal(encoding, &mon->lastSampledValue);
}

/* Has this sample changed from the last one? The method may allocate additional
 * space for the encoding buffer. Detect the change in encoding->data. */
static UA_Boolean
detectValueChange(UA_MonitoredItem *mon, UA_DataValue *value, UA_ByteString *encoding) {
    /* Apply Filter */
    UA_Boolean hasValue = value->hasValue;
    if(mon->filter.trigger == UA_DATACHANGETRIGGER_STATUS)
        value->hasValue = false;

    UA_Boolean hasServerTimestamp = value->hasServerTimestamp;
    UA_Boolean hasServerPicoseconds = value->hasServerPicoseconds;
    value->hasServerTimestamp = false;
    value->hasServerPicoseconds = false;

    UA_Boolean hasSourceTimestamp = value->hasSourceTimestamp;
    UA_Boolean hasSourcePicoseconds = value->hasSourcePicoseconds;
    if(mon->filter.trigger < UA_DATACHANGETRIGGER_STATUSVALUETIMESTAMP) {
        value->hasSourceTimestamp = false;
        value->hasSourcePicoseconds = false;
    }

    /* Detect the Value Change */
    UA_Boolean res = detectValueChangeWithFilter(mon, value, encoding);

    /* Reset the filter */
    value->hasValue = hasValue;
    value->hasServerTimestamp = hasServerTimestamp;
    value->hasServerPicoseconds = hasServerPicoseconds;
    value->hasSourceTimestamp = hasSourceTimestamp;
    value->hasSourcePicoseconds = hasSourcePicoseconds;
    return res;
}

/* Returns whether a new sample was created */
static UA_Boolean
sampleCallbackWithValue(UA_Server *server, UA_Subscription *sub,
                        UA_MonitoredItem *monitoredItem,
                        UA_DataValue *value,
                        UA_ByteString *valueEncoding) {
    UA_assert(monitoredItem->monitoredItemType == UA_MONITOREDITEMTYPE_CHANGENOTIFY);
    /* Store the pointer to the stack-allocated bytestring to see if a heap-allocation
     * was necessary */
    UA_Byte *stackValueEncoding = valueEncoding->data;

    /* Has the value changed? */
    UA_Boolean changed = detectValueChange(monitoredItem, value, valueEncoding);
    if(!changed)
        return false;

    /* Allocate the entry for the publish queue */
    UA_Notification *newNotification =
        (UA_Notification *)UA_malloc(sizeof(UA_Notification));
    if(!newNotification) {
        UA_LOG_WARNING_SESSION(server->config.logger, sub->session,
                               "Subscription %u | MonitoredItem %i | "
                               "Item for the publishing queue could not be allocated",
                               sub->subscriptionId, monitoredItem->monitoredItemId);
        return false;
    }

    /* Copy valueEncoding on the heap for the next comparison (if not already done) */
    if(valueEncoding->data == stackValueEncoding) {
        UA_ByteString cbs;
        if(UA_ByteString_copy(valueEncoding, &cbs) != UA_STATUSCODE_GOOD) {
            UA_LOG_WARNING_SESSION(server->config.logger, sub->session,
                                   "Subscription %u | MonitoredItem %i | "
                                   "ByteString to compare values could not be created",
                                   sub->subscriptionId, monitoredItem->monitoredItemId);
            UA_free(newNotification);
            return false;
        }
        *valueEncoding = cbs;
    }

    /* Prepare the newQueueItem */
    if(value->hasValue && value->value.storageType == UA_VARIANT_DATA_NODELETE) {
        /* Make a deep copy of the value */
        UA_StatusCode retval = UA_DataValue_copy(value, &newNotification->data.value);
        if(retval != UA_STATUSCODE_GOOD) {
            UA_LOG_WARNING_SESSION(server->config.logger, sub->session,
                                   "Subscription %u | MonitoredItem %i | "
                                   "Item for the publishing queue could not be prepared",
                                   sub->subscriptionId, monitoredItem->monitoredItemId);
            UA_free(newNotification);
            return false;
        }
    } else {
        newNotification->data.value = *value; /* Just copy the value and do not release it */
    }

    /* <-- Point of no return --> */

    UA_LOG_DEBUG_SESSION(server->config.logger, sub->session,
                         "Subscription %u | MonitoredItem %u | Sampled a new value",
                         sub->subscriptionId, monitoredItem->monitoredItemId);

    newNotification->mon = monitoredItem;

    /* Replace the encoding for comparison */
    UA_Variant_deleteMembers(&monitoredItem->lastValue);
    UA_Variant_copy(&value->value, &monitoredItem->lastValue);
    UA_ByteString_deleteMembers(&monitoredItem->lastSampledValue);
    monitoredItem->lastSampledValue = *valueEncoding;

    /* Add the notification to the end of local and global queue */
    TAILQ_INSERT_TAIL(&monitoredItem->queue, newNotification, listEntry);
    TAILQ_INSERT_TAIL(&sub->notificationQueue, newNotification, globalEntry);
    ++monitoredItem->queueSize;
    ++sub->notificationQueueSize;

    /* Remove some notifications if the queue is beyond maximum capacity */
    MonitoredItem_ensureQueueSpace(server, monitoredItem);

    return true;
}

void
UA_MonitoredItem_SampleCallback(UA_Server *server,
                                UA_MonitoredItem *monitoredItem) {
    UA_Subscription *sub = monitoredItem->subscription;
    if(monitoredItem->monitoredItemType != UA_MONITOREDITEMTYPE_CHANGENOTIFY) {
        UA_LOG_DEBUG_SESSION(server->config.logger, sub->session,
                             "Subscription %u | MonitoredItem %i | "
                             "Not a data change notification",
                             sub->subscriptionId, monitoredItem->monitoredItemId);
        return;
    }

    /* Read the value */
    UA_ReadValueId rvid;
    UA_ReadValueId_init(&rvid);
    rvid.nodeId = monitoredItem->monitoredNodeId;
    rvid.attributeId = monitoredItem->attributeId;
    rvid.indexRange = monitoredItem->indexRange;
    UA_DataValue value =
        UA_Server_readWithSession(server, sub->session,
                                  &rvid, monitoredItem->timestampsToReturn);

    /* Stack-allocate some memory for the value encoding. We might heap-allocate
     * more memory if needed. This is just enough for scalars and small
     * structures. */
    UA_STACKARRAY(UA_Byte, stackValueEncoding, UA_VALUENCODING_MAXSTACK);
    UA_ByteString valueEncoding;
    valueEncoding.data = stackValueEncoding;
    valueEncoding.length = UA_VALUENCODING_MAXSTACK;

    /* Create a sample and compare with the last value */
    UA_Boolean newNotification = sampleCallbackWithValue(server, sub, monitoredItem,
                                                         &value, &valueEncoding);

    /* Clean up */
    if(!newNotification) {
        if(valueEncoding.data != stackValueEncoding)
            UA_ByteString_deleteMembers(&valueEncoding);
        UA_DataValue_deleteMembers(&value);
    }
}

UA_StatusCode
MonitoredItem_registerSampleCallback(UA_Server *server, UA_MonitoredItem *mon) {
    if(mon->sampleCallbackIsRegistered)
        return UA_STATUSCODE_GOOD;
    UA_StatusCode retval =
        UA_Server_addRepeatedCallback(server, (UA_ServerCallback)UA_MonitoredItem_SampleCallback,
                                      mon, (UA_UInt32)mon->samplingInterval, &mon->sampleCallbackId);
    if(retval == UA_STATUSCODE_GOOD)
        mon->sampleCallbackIsRegistered = true;
    return retval;
}

UA_StatusCode
MonitoredItem_unregisterSampleCallback(UA_Server *server, UA_MonitoredItem *mon) {
    if(!mon->sampleCallbackIsRegistered)
        return UA_STATUSCODE_GOOD;
    mon->sampleCallbackIsRegistered = false;
    return UA_Server_removeRepeatedCallback(server, mon->sampleCallbackId);
}

#endif /* UA_ENABLE_SUBSCRIPTIONS */
