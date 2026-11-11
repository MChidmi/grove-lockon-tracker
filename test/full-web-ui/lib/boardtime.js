// The SD demo firmware's FAT clock runs on local wall-clock time (FAT has no time zones).
const tzOffsetS = () => -new Date().getTimezoneOffset() * 60;

// Epoch milliseconds -> the board's local seconds.
export const toBoardTime = (ms = Date.now()) => Math.floor(ms / 1000) + tzOffsetS();

// The board's local seconds -> epoch milliseconds (null when the file has no date).
export const fromBoardTime = (s) => (s ? (s - tzOffsetS()) * 1000 : null);
