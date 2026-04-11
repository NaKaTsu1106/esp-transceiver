"""グループ単位の送話権（Floor Control）管理。"""

_holders: dict[int, int] = {}  # group_id -> session_id (0 = 空き)


def request(group_id: int, session_id: int) -> bool:
    """送話権を要求する。取得できれば True。"""
    if _holders.get(group_id, 0) == 0:
        _holders[group_id] = session_id
        return True
    return False


def release(group_id: int, session_id: int) -> None:
    """送話権を解放する。"""
    if _holders.get(group_id) == session_id:
        _holders[group_id] = 0


def holder(group_id: int) -> int:
    """現在の送話権保持者の session_id を返す（0 = 空き）。"""
    return _holders.get(group_id, 0)
