#include "packet.h"

#include <library/cpp/actors/core/probes.h>

#include <util/system/datetime.h>

LWTRACE_USING(ACTORLIB_PROVIDER);

ui32 TEventHolder::Fill(IEventHandle& ev) {
    Serial = 0;
    Descr.Type = ev.Type;
    Descr.Flags = ev.Flags;
    Descr.Recipient = ev.Recipient;
    Descr.Sender = ev.Sender;
    Descr.Cookie = ev.Cookie;
    ev.TraceId.Serialize(&Descr.TraceId);
    ForwardRecipient = ev.GetForwardOnNondeliveryRecipient();
    EventActuallySerialized = 0;
    Descr.Checksum = 0;

    if (ev.HasBuffer()) {
        Buffer = ev.ReleaseChainBuffer();
        EventSerializedSize = Buffer->GetSize();
    } else if (ev.HasEvent()) {
        Event.Reset(ev.ReleaseBase());
        EventSerializedSize = Event->CalculateSerializedSize();
    } else {
        EventSerializedSize = 0;
    }

    return EventSerializedSize;
}
