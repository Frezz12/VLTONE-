package collab

import "vltstudio/backend/internal/objectstore"

const (
	UploadModeSingle    = "single"
	UploadModeMultipart = "multipart"

	MultipartStateNone       = "none"
	MultipartStateCreating   = "creating"
	MultipartStateOpen       = "open"
	MultipartStateCompleting = "completing"
	MultipartStateAssembled  = "assembled"
	MultipartStateAborting   = "aborting"
	MultipartStateAborted    = "aborted"

	multipartPartAlignment = int64(1 << 20)
)

type multipartPlan struct {
	Enabled  bool
	PartSize int64
	Parts    int
}

func planMultipart(totalBytes, thresholdBytes, preferredPartBytes int64) (multipartPlan, error) {
	if totalBytes <= 0 || thresholdBytes <= 0 || preferredPartBytes <= 0 {
		return multipartPlan{}, invalidf("multipart byte limits must be positive")
	}
	// Never let an operator-set threshold route an object beyond S3's
	// PutObject limit through the single-PUT path.
	if totalBytes < thresholdBytes && totalBytes <= objectstore.MaximumSinglePutBytes {
		return multipartPlan{}, nil
	}
	partSize := preferredPartBytes
	if partSize < objectstore.MinimumMultipartBytes {
		partSize = objectstore.MinimumMultipartBytes
	}
	requiredForCap := divideRoundUp(totalBytes, objectstore.MaximumMultipartParts)
	if requiredForCap > partSize {
		partSize = requiredForCap
	}
	partSize = alignRoundUp(partSize, multipartPartAlignment)
	if partSize >= totalBytes {
		// A multipart object must contain at least two parts. Treat the configured
		// size as a target and lower it only for this small threshold edge case.
		partSize = alignRoundUp(divideRoundUp(totalBytes, 2), multipartPartAlignment)
	}
	if partSize < objectstore.MinimumMultipartBytes ||
		partSize > objectstore.MaximumMultipartBytes || partSize >= totalBytes {
		return multipartPlan{}, invalidf("object cannot be represented by the bounded multipart layout")
	}
	parts := int(divideRoundUp(totalBytes, partSize))
	if parts < 2 || parts > objectstore.MaximumMultipartParts {
		return multipartPlan{}, invalidf("multipart part count is outside the supported range")
	}
	return multipartPlan{Enabled: true, PartSize: partSize, Parts: parts}, nil
}

func multipartPartBytes(totalBytes, partSize int64, partCount, partNumber int) (int64, error) {
	if totalBytes <= 0 || partSize < objectstore.MinimumMultipartBytes ||
		partSize > objectstore.MaximumMultipartBytes || partCount < 2 ||
		partCount > objectstore.MaximumMultipartParts || partNumber < 1 || partNumber > partCount {
		return 0, invalidf("multipart layout is invalid")
	}
	if partNumber < partCount {
		return partSize, nil
	}
	last := totalBytes - int64(partCount-1)*partSize
	if last <= 0 || last > partSize {
		return 0, invalidf("multipart final part size is invalid")
	}
	return last, nil
}

func divideRoundUp(value, divisor int64) int64 {
	return (value + divisor - 1) / divisor
}

func alignRoundUp(value, alignment int64) int64 {
	return ((value + alignment - 1) / alignment) * alignment
}

func missingMultipartPartPage(partCount, start, limit int,
	uploaded map[int]struct{}) ([]int, *int) {
	if partCount < 1 || start < 1 || start > partCount || limit < 1 {
		return nil, nil
	}
	parts := make([]int, 0, limit)
	for partNumber := start; partNumber <= partCount; partNumber++ {
		if _, exists := uploaded[partNumber]; exists {
			continue
		}
		if len(parts) == limit {
			next := partNumber
			return parts, &next
		}
		parts = append(parts, partNumber)
	}
	return parts, nil
}
