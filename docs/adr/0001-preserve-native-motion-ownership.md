# Preserve native motion ownership gaps

The original firmware suspends motion sampling while sound playback or IR owns the foreground and shared workspace, although ordinary menus and games continue sampling. Preserve those gaps when introducing sensor FIFO batching, rather than adding independent continuous step credit, because the user prefers the original behavior as part of fidelity. Batching must preserve samples from eligible intervals across wake and mode transitions without replaying excluded intervals into the estimator.
