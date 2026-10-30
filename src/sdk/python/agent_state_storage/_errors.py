"""Exceptions raised by the clients."""

from __future__ import annotations

import grpc


class AgentStateError(Exception):
    """A call to the agentstate.v1 service failed."""

    def __init__(self, message: str, code: grpc.StatusCode | None = None):
        super().__init__(message)
        self.code = code


class NotFound(AgentStateError):
    """The named session, record or blob does not exist."""


class PermissionDenied(AgentStateError):
    """The caller's identity does not allow this tenant."""


def translate(err: grpc.RpcError) -> AgentStateError:
    code = err.code() if hasattr(err, "code") else None
    message = err.details() if hasattr(err, "details") else str(err)
    if code == grpc.StatusCode.NOT_FOUND:
        return NotFound(message, code)
    if code in (grpc.StatusCode.PERMISSION_DENIED, grpc.StatusCode.UNAUTHENTICATED):
        return PermissionDenied(message, code)
    return AgentStateError(f"{code.name if code else 'ERROR'}: {message}", code)
