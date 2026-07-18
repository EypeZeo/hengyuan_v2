"""Base exception hierarchy for the HengYuan application.

All application-level exceptions inherit from ``HengYuanError`` so that
callers can catch them with a single broad clause when needed, while still
allowing fine-grained handling via subclass checks.

Usage::

    from app.core.exceptions import HengYuanError, InputValidationError

    raise InputValidationError("amount must be positive", details={"field": "amount"})

Design constraints:
  - Keep the hierarchy shallow — only add subclasses when a distinct HTTP
    status or a distinct handling branch is genuinely needed.
  - Do NOT embed database models, exchange objects, or framework-specific
    types here.  ``app/domain/**`` must not depend on this module either.
  - Subclasses added for Phase 2 business concepts should live in their
    respective domain or application module, not here.
"""

from __future__ import annotations


class HengYuanError(Exception):
    """Root exception for all HengYuan application errors.

    Args:
        message:    Human-readable description of the failure.
        error_code: Machine-readable identifier (UPPER_SNAKE_CASE).
                    Defaults to the class-level ``default_error_code``.
        details:    Optional mapping with additional structured context.
                    Must NOT contain secrets, tokens, or credentials.
    """

    default_error_code: str = "HENGYUAN_ERROR"

    def __init__(
        self,
        message: str,
        *,
        error_code: str | None = None,
        details: dict[str, object] | None = None,
    ) -> None:
        super().__init__(message)
        self.message: str = message
        self.error_code: str = error_code or self.__class__.default_error_code
        self.details: dict[str, object] = details or {}

    def __repr__(self) -> str:  # pragma: no cover
        return (
            f"{self.__class__.__name__}(error_code={self.error_code!r}, message={self.message!r})"
        )


# ---------------------------------------------------------------------------
# First-level subclasses — only what Phase 1 actually needs
# ---------------------------------------------------------------------------


class ConfigurationError(HengYuanError):
    """Raised when application configuration is invalid or incomplete.

    Typical HTTP mapping: 500 Internal Server Error (misconfigured server).
    """

    default_error_code: str = "CONFIGURATION_ERROR"


class InputValidationError(HengYuanError):
    """Raised when caller-supplied input fails validation.

    Distinct from Pydantic's ``ValidationError`` (which FastAPI handles
    automatically).  Use this for *business-level* input checks that happen
    after Pydantic parsing.

    Typical HTTP mapping: 422 Unprocessable Entity.
    """

    default_error_code: str = "INPUT_VALIDATION_ERROR"


class ApplicationError(HengYuanError):
    """Raised by the application / service orchestration layer.

    Use when a use-case or service cannot complete due to business-rule
    violations or orchestration failures that are not infrastructure issues.

    Typical HTTP mapping: 500 Internal Server Error.
    """

    default_error_code: str = "APPLICATION_ERROR"


class InfrastructureError(HengYuanError):
    """Raised when an external system (DB, exchange, scheduler, …) fails.

    Typical HTTP mapping: 503 Service Unavailable.
    """

    default_error_code: str = "INFRASTRUCTURE_ERROR"
